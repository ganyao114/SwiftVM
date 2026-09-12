#include "../support/case_support.h"
#include "../support/fp_environment.h"
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cmath>

TEST_CASE("AFP scalar unary and minmax preserve lanes under register reuse") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    const auto operation = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u);
    const bool reuse_second = GENERATE(false, true);
    const bool hardware_nan = GENERATE(false, true);
    CAPTURE(operation, reuse_second, hardware_nan);
    const bool unary = operation < 4;
    const auto sample = GENERATE_COPY(Catch::Generators::range(0u, unary ? 1u : 6u));
    CAPTURE(sample);
    const bool single = operation != 1 && operation < 6;
    const swift::u32 kind = operation < 2 ? 0 : operation - 1;
    const swift::u32 bits = single ? 32 : 64;
    constexpr swift::u32 output = sizeof(swift::x86::ThreadContext64);
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = output + 16,
            .arm64_features = Arm64Features::AFP,
            .sse_scalar_insert = true,
            .sse_afp_nan = hardware_nan,
    };
    AddressSpace address_space{config};
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::sse_afp_minmax, hardware_nan);
    auto module = address_space.MapModule(0x2d00, 0x2d01, module_config);
    IntrusivePtr<Block> block{new Block(0, Location{0x2d00})};
    auto first = block->LoadUniform(Uniform{
            offsetof(swift::x86::ThreadContext64, xmm0), ValueType::V128})
                         .SetType(ValueType::V128);
    auto second = block->LoadUniform(Uniform{
            offsetof(swift::x86::ThreadContext64, xmm1), ValueType::V128})
                          .SetType(ValueType::V128);
    auto result = Value{block->AppendInst(
            unary ? OpCode::VecFUnary : OpCode::VecFMinMax,
            first, second, Imm{bits}, Imm{unary ? kind : operation % 2},
            Imm{swift::u32{1}})}
                          .SetType(ValueType::V128);
    block->StoreUniform(Uniform{output, ValueType::V128}, result);
    block->SetTerminal(terminal::LinkBlock{Location{0x2e00}});
    block->ReIdInstr();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    RegAlloc alloc{block->MaxInstrId(),
                   address_space.GetTrampolines().GetGPRRegs(),
                   address_space.GetTrampolines().GetFPRRegs(), features, true};
    RegisterAllocPass::Run(block.get(), &alloc, true, features);
    REQUIRE(alloc.ValueFPR(first).id != alloc.ValueFPR(second).id);
    alloc.MapRegister(result.Id(), alloc.ValueFPR(reuse_second ? second : first));
    arm64::JitContext context{module, alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    auto [id, buffer] = module->AllocCodeCache(
            context.CurrentBufferSize(), context.RequiresRegionTrampoline());
    REQUIRE(id != INVALID_CACHE_ID);
    auto* code = context.Flush(buffer);
    Runtime runtime{&address_space};
    auto uniform = runtime.GetUniformBuffer();
    auto* guest = reinterpret_cast<swift::x86::ThreadContext64*>(uniform.data());
    guest->mxcsr = 0x1f80;
    guest->xmm0.l[0] = single ? 0x1122334440800000ull : 0x4010000000000000ull;
    guest->xmm0.l[1] = 0x5566778899aabbccull;
    guest->xmm1.l[0] = single ? 0xaabbccdd41100000ull : 0x4022000000000000ull;
    guest->xmm1.l[1] = 0xddeeff0011223344ull;
    if (sample != 0) {
        const swift::u64 sign = single ? 0x80000000ull : 0x8000000000000000ull;
        const swift::u64 quiet_nan = single ? 0x7fc12345ull : 0x7ff8000000012345ull;
        const swift::u64 signaling_nan = single ? 0x7f812345ull : 0x7ff0000000012345ull;
        swift::u64 first_bits = single ? guest->xmm0.i[0] : guest->xmm0.l[0];
        swift::u64 second_bits = single ? guest->xmm1.i[0] : guest->xmm1.l[0];
        if (sample == 1 || sample == 2) {
            first_bits = sample == 1 ? 0 : sign;
            second_bits = sample == 1 ? sign : 0;
        } else if (sample == 3) {
            first_bits = quiet_nan;
        } else {
            second_bits = sample == 4 ? quiet_nan : signaling_nan;
        }
        if (single) {
            guest->xmm0.i[0] = static_cast<swift::u32>(first_bits);
            guest->xmm1.i[0] = static_cast<swift::u32>(second_bits);
        } else {
            guest->xmm0.l[0] = first_bits;
            guest->xmm1.l[0] = second_bits;
        }
    }
    auto expected = unary ? guest->xmm1 : guest->xmm0;
    constexpr std::array<double, 8> answers{2.0, 2.0, 0.25, 0.5, 4.0, 9.0, 4.0, 9.0};
    if (sample != 0 && single) {
        expected.i[0] = guest->xmm1.i[0];
    } else if (sample != 0) {
        expected.l[0] = guest->xmm1.l[0];
    } else if (single) {
        const float value = static_cast<float>(answers[operation]);
        std::memcpy(&expected.i[0], &value, sizeof(value));
    } else {
        std::memcpy(&expected.l[0], &answers[operation], sizeof(double));
    }
    runtime.SetLocation(0x2d00);
    REQUIRE(address_space.GetTrampolines().GetRuntimeEntry()(
                    runtime.GetState(), code) == HaltReason::CodeMiss);
    swift::x86::Xmm actual{};
    std::memcpy(&actual, uniform.data() + output, sizeof(actual));
    if (operation == 2 || operation == 3) {
        float value;
        std::memcpy(&value, &actual.i[0], sizeof(value));
        REQUIRE(std::abs(value / answers[operation] - 1.0) <= 1.5 / 4096.0);
        REQUIRE(actual.i[1] == expected.i[1]);
    } else {
        REQUIRE(actual.l[0] == expected.l[0]);
    }
    REQUIRE(actual.l[1] == expected.l[1]);
#else
    SUCCEED("AFP execution requires an AArch64 host");
#endif
}

TEST_CASE("AFP scalar arithmetic may reuse the right input register") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    constexpr swift::u32 output = sizeof(swift::x86::ThreadContext64);
    constexpr std::array<double, 4> answers{10.0, 6.0, 16.0, 4.0};
    for (bool single : {false, true}) {
        for (unsigned operation = 0; operation < answers.size(); ++operation) {
            CAPTURE(single, operation);
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = output + 32,
                    .arm64_features = Arm64Features::AFP,
                    .sse_scalar_insert = true,
                    .sse_afp_nan = true,
            };
            AddressSpace address_space{config};
            auto module = address_space.GetDefaultModule();
            IntrusivePtr<Block> block{new Block(0, Location{0x2b00})};
            auto left = block->LoadUniform(Uniform{
                    offsetof(swift::x86::ThreadContext64, xmm0), ValueType::V128})
                                .SetType(ValueType::V128);
            auto right = block->LoadUniform(Uniform{
                    offsetof(swift::x86::ThreadContext64, xmm1), ValueType::V128})
                                 .SetType(ValueType::V128);
            constexpr std::array ops32{OpCode::VecFAddScalar32, OpCode::VecFSubScalar32,
                                       OpCode::VecFMulScalar32, OpCode::VecFDivScalar32};
            constexpr std::array ops64{OpCode::VecFAddScalar64, OpCode::VecFSubScalar64,
                                       OpCode::VecFMulScalar64, OpCode::VecFDivScalar64};
            auto result = Value{block->AppendInst(
                    (single ? ops32 : ops64)[operation], left, right)}
                                  .SetType(ValueType::V128);
            block->StoreUniform(Uniform{output, ValueType::V128}, result);
            block->StoreUniform(Uniform{output + 16, ValueType::V128}, left);
            block->SetTerminal(terminal::LinkBlock{Location{0x2c00}});
            block->ReIdInstr();
            const auto features = ResolveFeatureSet(module->GetModuleConfig());
            RegAlloc alloc{block->MaxInstrId(),
                           address_space.GetTrampolines().GetGPRRegs(),
                           address_space.GetTrampolines().GetFPRRegs(), features, true};
            RegisterAllocPass::Run(block.get(), &alloc, true, features);
            REQUIRE(alloc.ValueFPR(left).id != alloc.ValueFPR(right).id);
            alloc.MapRegister(result.Id(), alloc.ValueFPR(right));
            arm64::JitContext context{module, alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(block.get());
            auto [id, buffer] = module->AllocCodeCache(
                    context.CurrentBufferSize(), context.RequiresRegionTrampoline());
            REQUIRE(id != INVALID_CACHE_ID);
            auto* code = context.Flush(buffer);
            Runtime runtime{&address_space};
            auto uniform = runtime.GetUniformBuffer();
            auto* guest = reinterpret_cast<swift::x86::ThreadContext64*>(uniform.data());
            guest->mxcsr = 0x1f80;
            guest->xmm0.l[0] = single ? 0x1122334441000000ull : 0x4020000000000000ull;
            guest->xmm0.l[1] = 0x5566778899aabbccull;
            guest->xmm1.l[0] = single ? 0xaabbccdd40000000ull : 0x4000000000000000ull;
            guest->xmm1.l[1] = 0xddeeff0011223344ull;
            const auto original = guest->xmm0;
            auto expected = original;
            if (single) {
                const float value = static_cast<float>(answers[operation]);
                std::memcpy(&expected.i[0], &value, sizeof(value));
            } else {
                std::memcpy(&expected.l[0], &answers[operation], sizeof(double));
            }
            runtime.SetLocation(0x2b00);
            REQUIRE(address_space.GetTrampolines().GetRuntimeEntry()(
                            runtime.GetState(), code) == HaltReason::CodeMiss);
            swift::x86::Xmm actual{}, retained{};
            std::memcpy(&actual, uniform.data() + output, sizeof(actual));
            std::memcpy(&retained, uniform.data() + output + 16, sizeof(retained));
            REQUIRE(actual.l[0] == expected.l[0]);
            REQUIRE(actual.l[1] == expected.l[1]);
            REQUIRE(retained.l[0] == original.l[0]);
            REQUIRE(retained.l[1] == original.l[1]);
        }
    }
#else
    SUCCEED("AFP execution requires an AArch64 host");
#endif
}

TEST_CASE("AFP guest FPCR is rebuilt from MXCSR across host-call boundaries") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr std::uint32_t kResultBeforeOffset = sizeof(swift::x86::ThreadContext64);
    constexpr std::uint32_t kResultAfterOffset = kResultBeforeOffset + 16;
    constexpr std::uint32_t kHostFPCRSeenOffset = kResultAfterOffset + 16;
    constexpr std::uint32_t kDazResultOffset = kHostFPCRSeenOffset + 16;
    constexpr std::uint32_t kFtzResultOffset = kDazResultOffset + 16;
    constexpr std::uint32_t kHelperMutationResultOffset = kFtzResultOffset + 16;
    constexpr std::uint64_t kGuestPC = 0x2a00;
    constexpr std::uint32_t kMxcsrRoundUp = 0x1f80u | (2u << 13);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = sizeof(swift::x86::ThreadContext64) + 96,
            .arm64_features = Arm64Features::AFP,
            .sse_afp_nan = true,
    };
    AddressSpace address_space{config};

    auto* raw_block = new Block(0, Location{kGuestPC});
    IntrusivePtr<Block> block{raw_block};
    auto lhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm0), ValueType::V128});
    auto rhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm1), ValueType::V128});
    raw_block->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, mxcsr), ValueType::U32},
            raw_block->LoadImm(Imm{swift::u32{kMxcsrRoundUp}}).SetType(ValueType::U32));
    auto before = raw_block->VecFAddScalar32<TypedValue<ValueType::V128>>(lhs, rhs);
    raw_block->StoreUniform(Uniform{kResultBeforeOffset, ValueType::V128}, before);
    auto host_fpcr = raw_block
                             ->CallLambda(Lambda{Imm{swift::u64{
                                     reinterpret_cast<swift::VAddr>(
                                             FptrCast(&ReadFPCRFromHostHelper))}}})
                             .SetType(ValueType::U64);
    raw_block->StoreUniform(Uniform{kHostFPCRSeenOffset, ValueType::U64}, host_fpcr);
    auto after = raw_block->VecFAddScalar32<TypedValue<ValueType::V128>>(lhs, rhs);
    raw_block->StoreUniform(Uniform{kResultAfterOffset, ValueType::V128}, after);

    // Direct helpers are opaque: cold handlers and restore helpers may update
    // context.mxcsr.  Mutate it behind the JIT's back and prove that the
    // post-call cache comparison takes the miss path and rebuilds guest FPCR.
    auto context_address =
            raw_block->GetUniformAddress(Imm{swift::u64{0}}).SetType(ValueType::U64);
    auto nearest_mxcsr = raw_block->LoadImm(Imm{swift::u64{0x1f80}})
                                 .SetType(ValueType::U64);
    (void)raw_block
            ->CallLambda(
                    Lambda{Imm{swift::u64{reinterpret_cast<swift::VAddr>(
                            FptrCast(&WriteMXCSRFromHostHelper))}}},
                    context_address,
                    nearest_mxcsr)
            .SetType(ValueType::U64);
    auto after_helper_mutation =
            raw_block->VecFAddScalar32<TypedValue<ValueType::V128>>(lhs, rhs);
    raw_block->StoreUniform(
            Uniform{kHelperMutationResultOffset, ValueType::V128},
            after_helper_mutation);

    auto daz_lhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm2), ValueType::V128});
    auto daz_rhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm3), ValueType::V128});
    raw_block->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, mxcsr), ValueType::U32},
            raw_block->LoadImm(Imm{swift::u32{0x1fc0}}).SetType(ValueType::U32));
    auto daz_result =
            raw_block->VecFMulScalar32<TypedValue<ValueType::V128>>(daz_lhs, daz_rhs);
    raw_block->StoreUniform(Uniform{kDazResultOffset, ValueType::V128}, daz_result);

    auto ftz_lhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm4), ValueType::V128});
    auto ftz_rhs = raw_block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{offsetof(swift::x86::ThreadContext64, xmm5), ValueType::V128});
    raw_block->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, mxcsr), ValueType::U32},
            raw_block->LoadImm(Imm{swift::u32{0x9f80}}).SetType(ValueType::U32));
    auto ftz_result =
            raw_block->VecFMulScalar32<TypedValue<ValueType::V128>>(ftz_lhs, ftz_rhs);
    raw_block->StoreUniform(Uniform{kFtzResultOffset, ValueType::V128}, ftz_result);
    raw_block->SetTerminal(terminal::LinkBlock{Location{kGuestPC + 0x100}});
    raw_block->ReIdInstr();

    auto* code = TranslateIR(address_space.GetDefaultModule(), block);
    REQUIRE(code != nullptr);
    Runtime runtime{&address_space};
    auto uniform = runtime.GetUniformBuffer();
    const std::array<std::uint32_t, 4> lhs_bits{0x3f800000u, 0u, 0u, 0u};
    const std::array<std::uint32_t, 4> rhs_bits{0x33800000u, 0u, 0u, 0u};
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm0),
                lhs_bits.data(), sizeof(lhs_bits));
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm1),
                rhs_bits.data(), sizeof(rhs_bits));
    const std::array<std::uint32_t, 4> daz_lhs_bits{0x00000001u, 0u, 0u, 0u};
    const std::array<std::uint32_t, 4> daz_rhs_bits{0x4b000000u, 0u, 0u, 0u};
    const std::array<std::uint32_t, 4> ftz_lhs_bits{0x00800000u, 0u, 0u, 0u};
    const std::array<std::uint32_t, 4> ftz_rhs_bits{0x3f000000u, 0u, 0u, 0u};
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm2),
                daz_lhs_bits.data(), sizeof(daz_lhs_bits));
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm3),
                daz_rhs_bits.data(), sizeof(daz_rhs_bits));
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm4),
                ftz_lhs_bits.data(), sizeof(ftz_lhs_bits));
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, xmm5),
                ftz_rhs_bits.data(), sizeof(ftz_rhs_bits));

    // Deliberately install host-owned DN/FZ and a different rounding mode.
    // The helper must observe this value exactly, while guest arithmetic must
    // use MXCSR.RC=round-up and must not inherit either host bit.
    ScopedNativeFPCR host_fpcr_scope{(1ull << 25) | (1ull << 24) | (1ull << 23)};
    runtime.SetLocation(kGuestPC);
    const auto halt = address_space.GetTrampolines().GetRuntimeEntry()(
            runtime.GetState(), code);
    REQUIRE(halt == HaltReason::CodeMiss);

    std::uint32_t before_bits{};
    std::uint32_t after_bits{};
    std::uint32_t daz_bits{};
    std::uint32_t ftz_bits{};
    std::uint32_t helper_mutation_bits{};
    std::uint64_t helper_fpcr{};
    std::memcpy(&before_bits, uniform.data() + kResultBeforeOffset, sizeof(before_bits));
    std::memcpy(&helper_fpcr, uniform.data() + kHostFPCRSeenOffset, sizeof(helper_fpcr));
    std::memcpy(&after_bits, uniform.data() + kResultAfterOffset, sizeof(after_bits));
    std::memcpy(&daz_bits, uniform.data() + kDazResultOffset, sizeof(daz_bits));
    std::memcpy(&ftz_bits, uniform.data() + kFtzResultOffset, sizeof(ftz_bits));
    std::memcpy(&helper_mutation_bits,
                uniform.data() + kHelperMutationResultOffset,
                sizeof(helper_mutation_bits));
    REQUIRE(before_bits == 0x3f800001u);
    REQUIRE(after_bits == before_bits);
    REQUIRE(daz_bits == 0u);
    REQUIRE(ftz_bits == 0u);
    REQUIRE(helper_mutation_bits == 0x3f800000u);
    REQUIRE(helper_fpcr == host_fpcr_scope.Installed());
    REQUIRE(arm64::ReadNativeFPCR() == host_fpcr_scope.Installed());
#else
    SUCCEED("AFP FPCR execution check requires an AArch64 host");
#endif
}

TEST_CASE("AFP translated SSE arithmetic matches the 64-case x86 NaN truth matrix") {
    using namespace swift::runtime::backend;
    using namespace swift::translator::x86;

    struct Op {
        swift::u8 prefix;
        swift::u8 opcode;
        const char* name;
        bool f64;
    };
    constexpr std::array ops{
            Op{0xf3, 0x58, "addss", false}, Op{0xf3, 0x5c, "subss", false},
            Op{0xf3, 0x59, "mulss", false}, Op{0xf3, 0x5e, "divss", false},
            Op{0xf2, 0x58, "addsd", true},  Op{0xf2, 0x5c, "subsd", true},
            Op{0xf2, 0x59, "mulsd", true},  Op{0xf2, 0x5e, "divsd", true},
    };
    struct Pair {
        swift::u64 lhs;
        swift::u64 rhs;
        swift::u64 expected;
        const char* name;
    };
    constexpr std::array pairs32{
            Pair{0x7fc01234u, 0xffc05678u, 0x7fc01234u, "qq"},
            Pair{0x7fc01234u, 0xff802222u, 0x7fc01234u, "qs"},
            Pair{0x7f801111u, 0xffc05678u, 0x7fc01111u, "sq"},
            Pair{0x7f801111u, 0xff802222u, 0x7fc01111u, "ss"},
            Pair{0x7fc01234u, 0x3f800000u, 0x7fc01234u, "qn"},
            Pair{0x3f800000u, 0xffc05678u, 0xffc05678u, "nq"},
            Pair{0x7f801111u, 0x3f800000u, 0x7fc01111u, "sn"},
            Pair{0x3f800000u, 0xff802222u, 0xffc02222u, "ns"},
    };
    constexpr std::array pairs64{
            Pair{0x7ff8000000001234ull, 0xfff8000000005678ull,
                 0x7ff8000000001234ull, "qq"},
            Pair{0x7ff8000000001234ull, 0xfff0000000002222ull,
                 0x7ff8000000001234ull, "qs"},
            Pair{0x7ff0000000001111ull, 0xfff8000000005678ull,
                 0x7ff8000000001111ull, "sq"},
            Pair{0x7ff0000000001111ull, 0xfff0000000002222ull,
                 0x7ff8000000001111ull, "ss"},
            Pair{0x7ff8000000001234ull, 0x3ff0000000000000ull,
                 0x7ff8000000001234ull, "qn"},
            Pair{0x3ff0000000000000ull, 0xfff8000000005678ull,
                 0xfff8000000005678ull, "nq"},
            Pair{0x7ff0000000001111ull, 0x3ff0000000000000ull,
                 0x7ff8000000001111ull, "sn"},
            Pair{0x3ff0000000000000ull, 0xfff0000000002222ull,
                 0xfff8000000002222ull, "ns"},
    };

    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    auto* bytes = static_cast<swift::u8*>(guest_code);
    std::size_t case_index = 0;
    for (const auto& op : ops) {
        for (std::size_t pair_index = 0; pair_index < pairs32.size(); ++pair_index) {
            const std::size_t offset = case_index++ * 16;
            bytes[offset + 0] = op.prefix;
            bytes[offset + 1] = 0x0f;
            bytes[offset + 2] = op.opcode;
            bytes[offset + 3] = 0xc1;
            bytes[offset + 4] = 0xf4;
        }
    }

    SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    const auto& config = instance->GetAddressSpace()->GetConfig();
    const bool capable = swift::runtime::True(
            config.arm64_features & swift::runtime::Arm64Features::AFP);
    REQUIRE(config.sse_scalar_insert ==
            (swift::runtime::GetSvmConfig().sse_scalar_insert && capable));
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    struct Outcome {
        bool passed;
        const char* op;
        const char* pair;
        swift::u64 result;
    };
    std::vector<Outcome> outcomes;
    outcomes.reserve(64);
    case_index = 0;
    for (const auto& op : ops) {
        const auto& pairs = op.f64 ? pairs64 : pairs32;
        for (const auto& pair : pairs) {
            const std::size_t offset = case_index++ * 16;
            context.rip.qword = reinterpret_cast<swift::VAddr>(guest_code) + offset;
            context.xmm0 = swift::x86::Xmm{};
            context.xmm1 = swift::x86::Xmm{};
            context.xmm0.l[0] = pair.lhs;
            if (op.f64) {
                context.xmm0.l[1] = 0x99aabbcc55667788ull;
            } else {
                context.xmm0.i[1] = 0x11223344u;
                context.xmm0.i[2] = 0x55667788u;
                context.xmm0.i[3] = 0x99aabbccu;
            }
            context.xmm1.l[0] = pair.rhs;
            core->Run();
            const bool value_ok = op.f64
                                          ? context.xmm0.l[0] == pair.expected
                                          : context.xmm0.i[0] ==
                                                    static_cast<swift::u32>(pair.expected);
            const bool upper_ok = op.f64
                                          ? context.xmm0.l[1] == 0x99aabbcc55667788ull
                                          : context.xmm0.i[1] == 0x11223344u &&
                                                    context.xmm0.i[2] == 0x55667788u &&
                                                    context.xmm0.i[3] == 0x99aabbccu;
            outcomes.push_back(
                    Outcome{value_ok && upper_ok, op.name, pair.name, context.xmm0.l[0]});
        }
    }
    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);
    for (const auto& outcome : outcomes) {
        INFO(outcome.op << " " << outcome.pair << " result="
                        << fmt::format("{:016x}", outcome.result));
        REQUIRE(outcome.passed);
    }
}

TEST_CASE("AFP environment gate applies MXCSR changes through translated x86 code") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::translator::x86;

    // ldmxcsr [rip+5]; addss xmm0,xmm1; hlt; dd 0x5f80 (round-up)
    const std::array<swift::u8, 16> guest{
            0x0f, 0xae, 0x15, 0x05, 0x00, 0x00, 0x00,
            0xf3, 0x0f, 0x58, 0xc1, 0xf4,
            0x80, 0x5f, 0x00, 0x00,
    };
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, guest.data(), guest.size());

    SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    const auto& config = instance->GetAddressSpace()->GetConfig();
    const bool capable = True(config.arm64_features & Arm64Features::AFP);
    const bool effective = config.sse_afp_nan;
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<swift::VAddr>(guest_code);
    context.xmm0.i[0] = 0x3f800000u;
    context.xmm1.i[0] = 0x33800000u;
    const auto host_fpcr = arm64::ReadNativeFPCR();
    core->Run();
    const auto result = context.xmm0.i[0];
    const auto restored_fpcr = arm64::ReadNativeFPCR();
    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);

    const bool requested = swift::runtime::GetSvmConfig().sse_afp_nan;
    REQUIRE(effective == (requested && capable));
#if defined(__APPLE__)
    // Apple silicon exposes FEAT_AFP. This also prevents an ON full-suite run
    // from passing merely because capability detection silently fell back.
    REQUIRE(capable);
#endif
    REQUIRE(result == (effective ? 0x3f800001u : 0x3f800000u));
    REQUIRE(restored_fpcr == host_fpcr);
#else
    SUCCEED("AFP environment execution check requires an AArch64 host");
#endif
}

TEST_CASE("AFP mode terminates units after architectural MXCSR restores") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    auto check = [&](std::array<swift::u8, 16> bytes, bool xsave) {
        const char* old = swift::runtime::GetRawSvmConfigEnvForTest("SVM_XSAVE");
        const bool had_old = old != nullptr;
        const std::string old_value = old ? old : "";
        if (xsave) swift::runtime::SetSvmConfigEnvForTest("SVM_XSAVE", "1", 1);

        const auto address = reinterpret_cast<VAddr>(bytes.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::AFP, true, false, FeatureSet{}};
        decoder.Decode();

        if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_XSAVE", old_value.c_str(), 1);
        else swift::runtime::UnsetSvmConfigEnvForTest("SVM_XSAVE");

        const auto terminal = block.GetTerminal();
        REQUIRE(boost::get<terminal::ReturnToDispatch>(&terminal) != nullptr);
        std::size_t next_pc_stores = 0;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() != OpCode::SetLocation) continue;
            auto location = inst.GetArg<Lambda>(0);
            if (!location.IsValue() && location.GetImm().Get() == address + 3) {
                ++next_pc_stores;
            }
        }
        REQUIRE(next_pc_stores == 1);
    };

    auto instruction = [](swift::u8 modrm) {
        std::array<swift::u8, 16> bytes{};
        bytes[0] = 0x0f;
        bytes[1] = 0xae;
        bytes[2] = modrm;
        bytes[3] = 0xf4;
        return bytes;
    };

    SECTION("LDMXCSR") { check(instruction(0x10), false); }
    SECTION("FXRSTOR") { check(instruction(0x08), false); }
    SECTION("XRSTOR") { check(instruction(0x28), true); }
}

TEST_CASE("AFP mode brackets direct MemoryCopy host calls with native FPCR") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .arm64_features = Arm64Features::AFP,
            .sse_afp_nan = true,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x2b00})};
    block->MemoryCopy(Lambda{Imm{swift::u64{0x2000}}},
                      Lambda{Imm{swift::u64{0x1000}}}, Imm{swift::u32{16}});
    block->SetTerminal(terminal::LinkBlock{Location{0x2c00}});
    block->ReIdInstr();

    RegAlloc reg_alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(),
                       FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});
    arm64::JitContext context{address_space.GetDefaultModule(), reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();

    auto& masm = context.GetMasm();
    auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::size_t fpcr_writes = 0;
    std::size_t indirect_calls = 0;
    std::string text;
    for (const auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        const std::string_view line = disassembler.GetOutput();
        fpcr_writes += line.find("msr fpcr") != std::string_view::npos;
        indirect_calls += line.find("blr ") != std::string_view::npos;
        text += line;
        text += '\n';
    }
    INFO(text);
    REQUIRE(indirect_calls == 1);
    REQUIRE(fpcr_writes == 2);
}

TEST_CASE("AFP transparent helper calls retain guest FPCR without changing FPSR") {
#if defined(__aarch64__)
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    constexpr swift::u32 kResultOffset = sizeof(swift::x86::ThreadContext64);
    constexpr swift::u64 kGuestPC = 0x2c80;
    constexpr swift::u32 kMxcsrRoundUp = 0x1f80u | (2u << 13);
    constexpr swift::u64 kExpectedGuestFPCR =
            arm64::kSseAFPGuestFPCRBase | (swift::u64{1} << 22);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = sizeof(swift::x86::ThreadContext64) + 16,
            .arm64_features = Arm64Features::AFP,
            .sse_afp_nan = true,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{kGuestPC})};
    const auto result =
            block->CallLambda(
                         Lambda{DataClass{Imm{swift::u64{reinterpret_cast<swift::VAddr>(
                                        FptrCast(&ObserveFPEnvironmentFromTransparentHelper))}}},
                                HelperCallTraits{
                                        .host_fp = HostFpEffect::FPCRTransparent,
                                }})
                    .SetType(ValueType::U64);
    block->StoreUniform(Uniform{kResultOffset, ValueType::U64}, result);
    block->SetTerminal(terminal::LinkBlock{Location{kGuestPC + 0x100}});
    block->ReIdInstr();

    auto* code = TranslateIR(address_space.GetDefaultModule(), block);
    REQUIRE(code != nullptr);
    Runtime runtime{&address_space};
    auto uniform = runtime.GetUniformBuffer();
    std::memcpy(uniform.data() + offsetof(swift::x86::ThreadContext64, mxcsr),
                &kMxcsrRoundUp,
                sizeof(kMxcsrRoundUp));
    g_fpcr_transparent_helper_fpcr = 0;
    g_fpcr_transparent_helper_fpsr = 0;

    ScopedNativeFPCR host_fpcr_scope{(swift::u64{1} << 25) |
                                     (swift::u64{1} << 24) |
                                     (swift::u64{1} << 23)};
    ScopedNativeFPSR host_fpsr_scope{0x0800001full};
    runtime.SetLocation(kGuestPC);
    const auto halt = address_space.GetTrampolines().GetRuntimeEntry()(
            runtime.GetState(), code);
    REQUIRE(halt == HaltReason::CodeMiss);

    swift::u64 stored_result{};
    std::memcpy(&stored_result,
                uniform.data() + kResultOffset,
                sizeof(stored_result));
    REQUIRE(stored_result == swift::u64{0x5a5a5a5a5a5a5a5a});
    REQUIRE(g_fpcr_transparent_helper_fpcr == kExpectedGuestFPCR);
    REQUIRE(g_fpcr_transparent_helper_fpsr == host_fpsr_scope.Installed());
    REQUIRE(ReadNativeFPSR() == host_fpsr_scope.Installed());
    REQUIRE(arm64::ReadNativeFPCR() == host_fpcr_scope.Installed());
#else
    SUCCEED("AFP transparent-helper execution check requires an AArch64 host");
#endif
}

TEST_CASE("AFP transparent helper call shape omits only its FPCR switch pair") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto compile = [](HostFpEffect effect) {
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .arm64_features = Arm64Features::AFP,
                .sse_afp_nan = true,
        };
        AddressSpace address_space{config};
        IntrusivePtr<Block> block{new Block(0, Location{0x2cc0})};
        (void)block->CallLambda(
                Lambda{DataClass{Imm{swift::u64{reinterpret_cast<swift::VAddr>(
                               FptrCast(&ObserveFPEnvironmentFromTransparentHelper))}}},
                       HelperCallTraits{.host_fp = effect}});
        block->SetTerminal(terminal::LinkBlock{Location{0x2dc0}});
        block->ReIdInstr();

        RegAlloc reg_alloc{block->MaxInstrId(),
                           address_space.GetTrampolines().GetGPRRegs(),
                           address_space.GetTrampolines().GetFPRRegs(),
                           FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});
        arm64::JitContext context{address_space.GetDefaultModule(), reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();

        auto& masm = context.GetMasm();
        const auto* first =
                masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        const auto* last =
                masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        std::size_t fpcr_writes = 0;
        std::size_t indirect_calls = 0;
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            const std::string_view line = disassembler.GetOutput();
            fpcr_writes += line.find("msr fpcr") != std::string_view::npos;
            indirect_calls += line.find("blr ") != std::string_view::npos;
        }
        return std::pair{fpcr_writes, indirect_calls};
    };

    const auto conservative = compile(HostFpEffect::MayTouch);
    const auto transparent = compile(HostFpEffect::FPCRTransparent);
    REQUIRE(conservative.second == 1);
    REQUIRE(transparent.second == 1);
    REQUIRE(conservative.first == 2);
    REQUIRE(transparent.first == 0);
}

TEST_CASE("x87 FPCR-transparent action allowlist is exact and fails closed") {
    using namespace swift::x86;

    for (swift::u8 raw = static_cast<swift::u8>(X87Action::Init);
         raw <= static_cast<swift::u8>(X87Action::LoadEnvironment);
        ++raw) {
        const auto action = static_cast<X87Action>(raw);
        const bool expected = action == X87Action::LoadFloat ||
                              action == X87Action::StoreFloat ||
                              action == X87Action::StoreReg ||
                              action == X87Action::Remainder ||
                              action == X87Action::LoadConstant ||
                              action == X87Action::StoreControl ||
                              action == X87Action::StoreStatus;
        INFO("x87 action " << static_cast<unsigned>(raw));
        REQUIRE(X87ActionFPCRTransparent(action) == expected);
        REQUIRE(X87CommandFPCRTransparent(MakeX87Command(action)) == expected);
    }

    REQUIRE(X87DispatchFPFree(
                    0,
                    MakeX87Command(X87Action::Init),
                    0) == kX87GuestFault);
}

TEST_CASE("x87 FPCR-transparent dispatcher target is effective-AFP gated") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const char* old_jit = swift::runtime::GetRawSvmConfigEnvForTest("SVM_X87_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    swift::runtime::UnsetSvmConfigEnvForTest("SVM_X87_JIT");

    auto decode_target = [&](bool effective_afp) {
        // FLD1 is the audited LoadConstant action; HLT terminates the unit.
        std::array<swift::u8, 16> bytes{0xd9, 0xe8, 0xf4};
        const auto address = reinterpret_cast<VAddr>(bytes.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true,
                           effective_afp ? Arm64Features::AFP
                                         : Arm64Features::None,
                           effective_afp, false, FeatureSet{}};
        decoder.Decode();

        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() != OpCode::CallLambda) continue;
            const auto lambda = inst.GetArg<Lambda>(0);
            if (lambda.IsValue()) continue;
            const auto target = lambda.GetImm().Get();
            if (target == reinterpret_cast<VAddr>(&X87Dispatch) ||
                target == reinterpret_cast<VAddr>(&X87DispatchFPFree)) {
                return std::pair{target, lambda.GetHostFpEffect()};
            }
        }
        FAIL("FLD1 did not emit an x87 helper call");
        return std::pair{VAddr{0}, HostFpEffect::MayTouch};
    };

    const auto off = decode_target(false);
    const auto on = decode_target(true);
    if (had_old_jit) swift::runtime::SetSvmConfigEnvForTest("SVM_X87_JIT", old_jit_value.c_str(), 1);
    else swift::runtime::UnsetSvmConfigEnvForTest("SVM_X87_JIT");

    REQUIRE(off.first == reinterpret_cast<VAddr>(&X87Dispatch));
    REQUIRE(off.second == HostFpEffect::MayTouch);
    REQUIRE(on.first == reinterpret_cast<VAddr>(&X87DispatchFPFree));
    REQUIRE(on.second == HostFpEffect::FPCRTransparent);
}
