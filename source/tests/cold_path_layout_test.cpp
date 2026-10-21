#include <array>
#include <cstring>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/alloc/target_allocation_planner.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/register_alloc_pass.h"

TEST_CASE("function cold paths follow every hot block",
          "[arm64][codegen][cold-path]") {
#if defined(__aarch64__)
    using namespace swift;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr VAddr first_guest = 0x7300;
    constexpr VAddr second_guest = 0x7310;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    ModuleConfig module_config{};
    auto module = address_space.MapModule(
            LocationDescriptor{first_guest},
            LocationDescriptor{second_guest + 0x10},
            module_config);
    const auto features = ResolveFeatureSet(module_config);

    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(
            Location{first_guest}, Location{second_guest + 1});
    const auto left = function
                              ->LoadUniform<TypedValue<ValueType::V128>>(
                                      Uniform{0, ValueType::V128})
                              .SetType(ValueType::V128);
    const auto right = function
                               ->LoadUniform<TypedValue<ValueType::V128>>(
                                       Uniform{16, ValueType::V128})
                               .SetType(ValueType::V128);
    const auto result = function->VecFAddScalar32(left, right)
                                .SetType(ValueType::V128);
    function->StoreUniform(Uniform{32, ValueType::V128}, result);
    auto* second_block =
            builder.LinkBlock(terminal::LinkBlock{Location{second_guest}});
    builder.SetCurBlock(second_block);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    RegAlloc alloc{function->MaxInstrCount(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(),
                   features};
    RegisterAllocPass::Run(function, &alloc, features);
    alloc.CompleteGPRAllocation(module);
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(function);
    context.Finish();

    const auto second_offset = static_cast<u32>(
            context.GetLabel(LocationDescriptor{second_guest})->GetLocation());
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
    std::optional<u32> cold_target;
    for (u32 offset = 0; offset < second_offset; offset += sizeof(u32)) {
        u32 instruction{};
        std::memcpy(&instruction, bytes + offset, sizeof(instruction));
        if ((instruction & 0xff00'001fu) != 0x5400'0006u) {
            continue;
        }
        s32 immediate = static_cast<s32>((instruction >> 5) & 0x7ffffu);
        if (immediate & 0x40000) {
            immediate |= ~0x7ffff;
        }
        cold_target = static_cast<u32>(
                static_cast<s32>(offset) + immediate * sizeof(u32));
        break;
    }

    REQUIRE(cold_target);
    REQUIRE(*cold_target > second_offset);
    REQUIRE(*cold_target < context.CurrentBufferSize());
#else
    SUCCEED("ARM64 cold-path layout requires an AArch64 host");
#endif
}

TEST_CASE("function code objects emit independently allocated regions",
          "[arm64][codegen][function-code-object]") {
#if defined(__aarch64__)
    using namespace swift;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr VAddr first_guest = 0x7400;
    constexpr VAddr second_guest = 0x7500;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    AddressSpace address_space{config};
    ModuleConfig module_config{};
    auto module = address_space.MapModule(
            LocationDescriptor{first_guest},
            LocationDescriptor{second_guest + 0x10},
            module_config);
    const auto features = ResolveFeatureSet(module_config);

    HIRBuilder builder{2, true, features};
    auto make_region = [&](VAddr guest) {
        auto* function = builder.AppendFunction(Location{guest},
                                                Location{guest + 1});
        const auto value =
                function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{0, ValueType::U64});
        function->StoreUniform(Uniform{8, ValueType::U64}, value);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        return function;
    };
    auto* first = make_region(first_guest);
    auto* second = make_region(second_guest);

    RegAlloc first_alloc{first->MaxInstrCount(),
                         address_space.GetTrampolines().GetGPRRegs(),
                         address_space.GetTrampolines().GetFPRRegs(),
                         features};
    RegAlloc second_alloc{second->MaxInstrCount(),
                          address_space.GetTrampolines().GetGPRRegs(),
                          address_space.GetTrampolines().GetFPRRegs(),
                          features};
    RegisterAllocPass::Run(first, &first_alloc, features);
    RegisterAllocPass::Run(second, &second_alloc, features);
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, first_alloc, nullptr, first));
    REQUIRE(arm64::CompleteARM64UnitForEmission(module, second_alloc, nullptr, second));

    const std::array<arm64::FunctionRegionEmission, 2> regions{{
            {.function = first, .reg_alloc = &first_alloc},
            {.function = second, .reg_alloc = &second_alloc},
    }};
    arm64::FunctionCodeObjectEmitter emitter{module, regions};
    emitter.Emit(false);

    const auto first_offset = emitter.Context().GetCodeOffset(first_guest);
    const auto second_offset = emitter.Context().GetCodeOffset(second_guest);
    REQUIRE(first_offset >= 0);
    REQUIRE(second_offset > first_offset);
    REQUIRE(static_cast<u32>(second_offset) < emitter.CurrentBufferSize());
    REQUIRE(first->GetFunction()->TakeBlocksFrom(*second->GetFunction()));
    REQUIRE(first->GetFunction()->FindBlock(Location{second_guest}) != nullptr);
    REQUIRE(second->GetFunction()->GetBlocks().empty());
#else
    SUCCEED();
#endif
}

TEST_CASE("function code object ownership keeps external placeholders separate",
          "[function-code-object]") {
    using namespace swift;
    using namespace swift::runtime;
    using namespace swift::runtime::ir;

    constexpr VAddr first_guest = 0x7580;
    constexpr VAddr second_guest = 0x7590;
    HIRBuilder builder{2, true, {}};
    auto* first = builder.AppendFunction(Location{first_guest},
                                         Location{first_guest + 1});
    first->EndBlock(terminal::ReturnToHost{});
    first->EndFunction();

    auto* second = builder.AppendFunction(Location{second_guest},
                                          Location{second_guest + 1});
    second->EndBlock(terminal::ReturnToHost{});
    second->CreateOrGetBlock(Location{first_guest});
    second->EndFunction();

    REQUIRE(first->GetFunction()->TakeBlocksFrom(*second->GetFunction()));
    REQUIRE(first->GetFunction()->FindBlock(Location{first_guest}) != nullptr);
    REQUIRE(first->GetFunction()->FindBlock(Location{second_guest}) != nullptr);
    auto* placeholder = second->GetFunction()->FindBlock(Location{first_guest});
    REQUIRE(placeholder != nullptr);
    REQUIRE(placeholder->GetInstList().empty());
    REQUIRE_FALSE(placeholder->HasTerminal());
}

TEST_CASE("function code objects publish regions through one owner",
          "[arm64][codegen][function-code-object][publication]") {
#if defined(__aarch64__)
    using namespace swift;
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr VAddr first_guest = 0x7600;
    constexpr VAddr second_guest = 0x7700;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::All,
    };
    config.uniform_buffer_size = 256;
    AddressSpace address_space{config};
    ModuleConfig module_config{
            .read_only = true,
            .optimizations = Optimizations::All,
    };
    auto module = address_space.MapModule(LocationDescriptor{first_guest},
                                          LocationDescriptor{second_guest + 0x10},
                                          module_config);
    const auto features = ResolveFeatureSet(module_config);

    HIRBuilder builder{2, true, features};
    auto make_region = [&](VAddr guest) {
        auto* function = builder.AppendFunction(Location{guest}, Location{guest + 1});
        const auto value =
                function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{0, ValueType::U64});
        function->StoreUniform(Uniform{8, ValueType::U64}, value);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        return function;
    };
    auto* first = make_region(first_guest);
    auto* second = make_region(second_guest);
    const std::array<HIRFunction*, 2> regions{first, second};

    REQUIRE(TranslateIR(module, regions) != nullptr);
    auto* first_code = address_space.GetCodeCache(first_guest);
    auto* second_code = address_space.GetCodeCache(second_guest);
    REQUIRE(first_code != nullptr);
    REQUIRE(second_code != nullptr);
    REQUIRE(second_code != first_code);
    REQUIRE(first->GetFunction()->FindBlock(Location{first_guest}) != nullptr);
    REQUIRE(first->GetFunction()->FindBlock(Location{second_guest}) != nullptr);
    REQUIRE(second->GetFunction()->GetBlocks().empty());
#else
    SUCCEED();
#endif
}
