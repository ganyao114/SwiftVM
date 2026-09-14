#include "../support/case_support.h"
#include "../support/fp_environment.h"

TEST_CASE("Linux guest memory launch policy defaults to direct") {
    using swift::linux::SelectGuestMemoryLaunchPolicy;

    REQUIRE(SelectGuestMemoryLaunchPolicy(true, nullptr, nullptr).direct);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "0", nullptr).direct);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "OFF", nullptr).direct);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "off", nullptr).direct);
    REQUIRE(SelectGuestMemoryLaunchPolicy(true, "1", nullptr).direct);

    // Any explicit window-size request wins over the direct selector.
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, nullptr, "32").direct);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "1", "36").direct);

    // macOS passes linux_host=false and ignores the direct selector.
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(false, nullptr, nullptr).direct);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(false, "1", nullptr).direct);
}

TEST_CASE("direct image collision performs one bounded-bias fallback") {
    using swift::linux::GuestMemory;

    int direct_attempts = 0;
    int fallback_attempts = 0;
    REQUIRE(GuestMemory::TryDirectWithFallback(
            [&] {
                ++direct_attempts;
                return false;
            },
            [&] {
                ++fallback_attempts;
                return true;
            }));
    REQUIRE(direct_attempts == 1);
    REQUIRE(fallback_attempts == 1);

    direct_attempts = 0;
    fallback_attempts = 0;
    REQUIRE(GuestMemory::TryDirectWithFallback(
            [&] {
                ++direct_attempts;
                return true;
            },
            [&] {
                ++fallback_attempts;
                return false;
            }));
    REQUIRE(direct_attempts == 1);
    REQUIRE(fallback_attempts == 0);
}

TEST_CASE("direct guest unmap preserves untracked host mappings") {
    using swift::linux::GuestMemory;

#if defined(__APPLE__)
    STATIC_REQUIRE(GuestMemory::kHostPageSize == 0x4000);
#else
    STATIC_REQUIRE(GuestMemory::kHostPageSize == GuestMemory::kGuestPageSize);
#endif
    constexpr size_t size = GuestMemory::kHostPageSize;
    auto* host = static_cast<swift::u8*>(
            mmap(nullptr,
                 size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 -1,
                 0));
    REQUIRE(host != MAP_FAILED);
    host[0] = 0x5a;

    GuestMemory memory;
    memory.EnableDirectMode();
    const auto untracked = reinterpret_cast<swift::VAddr>(host);
    REQUIRE_FALSE(memory.RangeIsMapped(untracked, size));
    memory.Unmap(untracked, size);

    // Linux 允许对 guest 未映射区间执行 munmap。恒等模式下它必须保持 guest
    // no-op，不能误拆翻译器拥有的同数值宿主映射。
    host[0] ^= 0xff;
    REQUIRE(host[0] == 0xa5);
    REQUIRE(mprotect(host, size, PROT_READ | PROT_WRITE) == 0);
    REQUIRE(munmap(host, size) == 0);

    const auto tracked = memory.MapAnywhere(size);
    REQUIRE(tracked != 0);
    REQUIRE(memory.RangeIsMapped(tracked, size));
    memory.Unmap(tracked, size);
    REQUIRE_FALSE(memory.RangeIsMapped(tracked, size));
}

TEST_CASE("hot coalesce check classifies static opportunities") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;

    REQUIRE(PerfStats2::kGetenvNames.size() == 155);
    REQUIRE(PerfStats2::kGetenvNames.size() == kSvmConfigFieldCount);
    REQUIRE(std::string_view(PerfStats2::kGetenvNames.front()) ==
            "SVM_MEM_DIRECT");
    REQUIRE(std::string_view(PerfStats2::kGetenvNames.back()) ==
            "SVM_RA_FIXED_CLASS");
    STATIC_REQUIRE(offsetof(backend::RuntimeProfileInterface, exec) == 0);

    REQUIRE(HotCoalesceIsMoveBridge("mov x1, x2"));
    REQUIRE(HotCoalesceIsMoveBridge("  fmov d0, x3"));
    REQUIRE(HotCoalesceIsMoveBridge("ubfx x4, x5, #0x0, #0x20"));
    REQUIRE(HotCoalesceIsMoveBridge("lsl x1, x2, #0"));
    REQUIRE_FALSE(HotCoalesceIsMoveBridge("add x1, x2, x3"));
    REQUIRE_FALSE(HotCoalesceIsMoveBridge("lsl x1, x2, #1"));

    Block block{0, Location{0x7100}};
    Assembler assembler{&block};
    for (swift::u32 offset : {0u, 8u, 16u, 24u}) {
        (void)assembler.LoadUniform<U64>(Uniform{offset, ValueType::U64});
    }
    const auto stored = assembler.LoadImm<U64>(Imm{1});
    assembler.StoreUniform(Uniform{32, ValueType::U64}, stored);
    assembler.StoreUniform(Uniform{40, ValueType::U64}, stored);
    (void)assembler.LoadImm<U64>(Imm{2});
    (void)assembler.LoadUniform<U64>(Uniform{64, ValueType::U64});
    (void)assembler.LoadUniform<U64>(Uniform{64, ValueType::U64});

    const auto stats = HotCoalesceAnalyzeUniformSequences(&block);
    REQUIRE(stats.sequences == 2);
    REQUIRE(stats.load_pairs == 2);
    REQUIRE(stats.store_pairs == 1);
    REQUIRE(stats.same_offset == 1);
    REQUIRE(stats.saved_instructions == 4);
}

TEST_CASE("helper effects default conservative and compose with ABI metadata") {
    using namespace swift::runtime::ir;

    const auto address = DataClass{Imm{swift::u64{0x1234}}};
    const Lambda ordinary{address};
    REQUIRE(ordinary.GetHostFpEffect() == HostFpEffect::MayTouch);
    REQUIRE(ordinary.GetHostRegisterEffect() ==
            HostRegisterEffect::MayTouchSIMD);
    REQUIRE(ordinary.GetHelperABI() == HelperABI::NormalAAPCS);
    REQUIRE(ordinary.GetUniformEffectId() == UniformEffectId::Unknown);
    REQUIRE(ordinary.GetHelperGuestStateEffect() ==
            HelperGuestStateEffect::MayReadWrite);
    REQUIRE(ordinary.GetHelperFaultEffect() == HelperFaultEffect::MayFault);
    REQUIRE(ordinary.GetHelperReentryEffect() == HelperReentryEffect::MayReenter);
    REQUIRE(ordinary.GetHostFlagsEffect() == HostFlagsEffect::MayTouch);

    const Lambda combined{
            address,
            HelperCallTraits{
                    .uniform = UniformEffectId::None,
                    .abi = HelperABI::PreserveAllLeaf,
                    .host_fp = HostFpEffect::FPCRTransparent,
                    .host_registers = HostRegisterEffect::GeneralOnly,
                    .guest_state = HelperGuestStateEffect::ReadOnly,
                    .fault = HelperFaultEffect::NoDirectFault,
                    .reentry = HelperReentryEffect::NoReentry,
                    .host_flags = HostFlagsEffect::PreservesNZCV,
            }};
    REQUIRE(combined.GetImm().Get() == swift::u64{0x1234});
    REQUIRE(combined.GetHostFpEffect() == HostFpEffect::FPCRTransparent);
    REQUIRE(combined.GetHostRegisterEffect() ==
            HostRegisterEffect::GeneralOnly);
    REQUIRE(combined.GetHelperABI() == HelperABI::PreserveAllLeaf);
    REQUIRE(combined.GetUniformEffectId() == UniformEffectId::None);
    REQUIRE(combined.GetHelperGuestStateEffect() == HelperGuestStateEffect::ReadOnly);
    REQUIRE(combined.GetHelperFaultEffect() == HelperFaultEffect::NoDirectFault);
    REQUIRE(combined.GetHelperReentryEffect() == HelperReentryEffect::NoReentry);
    REQUIRE(combined.GetHostFlagsEffect() == HostFlagsEffect::PreservesNZCV);

    const Lambda resident{
            address,
            HelperCallTraits{
                    .host_fp = HostFpEffect::FPCRTransparent,
                    .host_registers = HostRegisterEffect::PreservesPinnedState,
            }};
    REQUIRE(resident.GetHostFpEffect() == HostFpEffect::FPCRTransparent);
    REQUIRE(resident.GetHostRegisterEffect() ==
            HostRegisterEffect::PreservesPinnedState);
    REQUIRE(resident.GetHelperABI() == HelperABI::NormalAAPCS);
    REQUIRE(resident.GetUniformEffectId() == UniformEffectId::Unknown);
}

TEST_CASE("config hash includes programmatic effective AFP policy") {
    swift::runtime::Config off{};
    swift::runtime::Config on{};
    on.sse_scalar_insert = true;

    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) !=
            swift::runtime::backend::ComputeConfigHash(on));
    on.sse_scalar_insert = false;
    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) ==
            swift::runtime::backend::ComputeConfigHash(on));
    on.sse_afp_nan = true;

    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) !=
            swift::runtime::backend::ComputeConfigHash(on));
    on.sse_afp_nan = false;
    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) ==
            swift::runtime::backend::ComputeConfigHash(on));
}

TEST_CASE("FeatureSet captures every B-class field and applies sparse overrides") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;

    STATIC_REQUIRE(kFeatureCount == 70);
    REQUIRE(FeatureSet{}.ra_coalesce_live);
    REQUIRE(FeatureSet{}.operand_copy_kill);
    REQUIRE(FeatureSet{}.zero_store_zr);
    REQUIRE_FALSE(FeatureSet{}.flags_regs_audit);
    REQUIRE_FALSE(FeatureSet{}.ra_fixed_class);
    REQUIRE(FlagsRegsEnabled() == GetSvmConfig().flags_regs);
    STATIC_REQUIRE(kFlagsNzcvParkSlot == 0);
    STATIC_REQUIRE(kFlagsResultParkSlot == 1);
    STATIC_REQUIRE(state_offset_flags_nzcv_park == state_offset_spill_area);
    STATIC_REQUIRE(state_offset_flags_result_park ==
                   state_offset_spill_area + 8);
    const auto& svm = GetSvmConfig();
    const auto capture = svm.GetFeatureSet();
#define CHECK_FEATURE_COPY(field, default_value) REQUIRE(capture.field == svm.field);
    SVM_FEATURE_FIELDS(CHECK_FEATURE_COPY)
#undef CHECK_FEATURE_COPY

    ModuleConfig empty{};
    const auto resolved_empty = ResolveFeatureSet(empty);
#define CHECK_EMPTY_RESOLVE(field, default_value) \
    REQUIRE(resolved_empty.field == capture.field);
    SVM_FEATURE_FIELDS(CHECK_EMPTY_RESOLVE)
#undef CHECK_EMPTY_RESOLVE

    ModuleConfig overridden{};
    overridden.feature_overrides.Set(FeatureId::const_cse, !capture.const_cse);
    const auto resolved_override = ResolveFeatureSet(overridden);
    REQUIRE(resolved_override.const_cse != capture.const_cse);
    REQUIRE(resolved_override.uniform_dse == capture.uniform_dse);

    Config config{};
    REQUIRE(ComputeConfigHash(config, empty) == ComputeConfigHash(config));
    REQUIRE(ComputeConfigHash(config, overridden) != ComputeConfigHash(config));
}

TEST_CASE("loop invariant hoist keeps a narrow unit-local proof boundary") {
    using namespace swift::runtime::ir;

    FeatureSet features{};
    features.loop_gpr_hoist = true;
    features.loop_const_hoist = true;
    UniformInfo info{};
    info.loop_gpr_uniform_ranges.push_back({0, 16 * sizeof(swift::u64)});

    HIRBuilder builder{1, true, features};
    constexpr Location loop_pc{0x1750};
    auto* function = builder.AppendFunction(loop_pc, Location{0x1760});
    const auto base0 = function->LoadUniform(Uniform{104, ValueType::U64});
    const auto base1 = function->LoadUniform(Uniform{112, ValueType::U64});
    const auto index = function->LoadImm(Imm{16u}).SetType(ValueType::U64);
    const auto address0 = function->Add(base0, Operand{index}).SetType(ValueType::U64);
    const auto address1 = function->Add(base1, Operand{index}).SetType(ValueType::U64);
    const auto loaded0 = function->LoadMemory(Operand{address0}).SetType(ValueType::U64);
    const auto loaded1 = function->LoadMemory(Operand{address1}).SetType(ValueType::U64);
    (void)loaded0;
    (void)loaded1;
    const auto loop_end = function->LoadImm(Imm{swift::u64{0x1312d000}})
                                  .SetType(ValueType::U64);
    const auto compare = function->Sub(index, Operand{loop_end}).SetType(ValueType::U64);
    function->SaveFlags(compare, Flags::NZCV);
    function->EndBlock(terminal::Condition{
            Cond::NE, terminal::LinkBlock{loop_pc}, terminal::ReturnToDispatch{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();

    auto* block = function->GetHIRBlocksRPO().front().GetBlock();
    std::vector<Inst*> original;
    for (auto& inst : block->GetInstList()) original.push_back(&inst);
    auto recipe = LoopInvariantHoistRecipe::Analyze(function, info, features);
    REQUIRE_FALSE(recipe->Empty());
    recipe->Apply();
    const auto& metadata = block->GetLoopHoistMetadata();
    REQUIRE(metadata.gpr_count == 1);
    REQUIRE(metadata.const_count == 1);
    REQUIRE(metadata.anchors.size() == 2);
    REQUIRE(metadata.anchors.front()->GetOp() == OpCode::LoadUniform);
    REQUIRE(metadata.anchors.back()->GetOp() == OpCode::LoadImm);

    recipe->Revert();
    REQUIRE(block->GetLoopHoistMetadata().anchors.empty());
    std::vector<Inst*> restored;
    for (auto& inst : block->GetInstList()) restored.push_back(&inst);
    REQUIRE(restored == original);

    HIRBuilder written_builder{1, true, features};
    auto* written = written_builder.AppendFunction(loop_pc, Location{0x1760});
    const auto old_base = written->LoadUniform(Uniform{104, ValueType::U64});
    const auto new_base = written->Add(old_base, Operand{Imm{8u}}).SetType(ValueType::U64);
    written->StoreUniform(Uniform{104, ValueType::U64}, new_base);
    written->EndBlock(terminal::LinkBlock{loop_pc});
    written->EndFunction();
    written->ComputeRPO();
    written->IdByRPO();
    FeatureSet gpr_only = features;
    gpr_only.loop_const_hoist = false;
    auto rejected = LoopInvariantHoistRecipe::Analyze(written, info, gpr_only);
    REQUIRE(rejected->Empty());
}

TEST_CASE("FPR demand-lean prices proven peaks and reclaims only AFP cold ABI") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::backend::arm64;
    using namespace swift::runtime::ir;

    IntrusivePtr<Block> block{new Block(0, Location{0x7100})};
    auto left = block->LoadUniform(
            Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto right = block->LoadUniform(
            Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto scalar32 = block->VecFAddScalar32(left, right).SetType(ValueType::V128);
    auto scalar64 = block->VecFAddScalar64(left, right).SetType(ValueType::V128);
    auto convert_fast = block->VecFCvtPacked(left, Imm{swift::u64{1}})
                                .SetType(ValueType::V128);
    auto convert_wide = block->VecFCvtPacked(left, Imm{swift::u64{2}})
                                .SetType(ValueType::V128);

    FeatureSet off{};
    off.fpr_scratch_precise = false;
    FeatureSet on{};
    on.fpr_scratch_precise = true;
    REQUIRE(ScratchBudget(*scalar32.Def(), off).fpr == 5);
    REQUIRE(ScratchBudget(*scalar64.Def(), off).fpr == 4);
    REQUIRE(ScratchBudget(*convert_fast.Def(), off).fpr == 5);
    REQUIRE(ScratchBudget(*scalar32.Def(), on).fpr == 1);
    REQUIRE(ScratchBudget(*scalar64.Def(), on).fpr == 0);
    REQUIRE(ScratchBudget(*convert_fast.Def(), on).fpr == 1);
    REQUIRE(ScratchBudget(*convert_wide.Def(), on).fpr == 5);

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .backend_isa = kArm64,
            .uniform_buffer_size = 4096,
            .sse_afp_nan = true,
    };
    FeatureSet keep{};
    keep.fpr_ipv_reclaim = false;
    FeatureSet reclaim{};
    reclaim.fpr_ipv_reclaim = true;
    TrampolinesArm64 trampolines{config, keep};
    RegAlloc kept{1, trampolines.GetGPRRegs(), trampolines.GetFPRRegs(), keep,
                  true};
    RegAlloc reclaimed{1, trampolines.GetGPRRegs(), trampolines.GetFPRRegs(),
                       reclaim, true};
    auto kept_fprs = kept.GetFprs();
    auto reclaimed_fprs = reclaimed.GetFprs();
    REQUIRE(kept_fprs.GetClearCount() == 28);
    REQUIRE(reclaimed_fprs.GetClearCount() == 32);

    config.sse_afp_nan = false;
    TrampolinesArm64 guarded{config, reclaim};
    RegAlloc non_afp{1, guarded.GetGPRRegs(), guarded.GetFPRRegs(), reclaim,
                     false};
    auto non_afp_fprs = non_afp.GetFprs();
    REQUIRE(non_afp_fprs.GetClearCount() == 28);
}

TEST_CASE("module feature binding parser accepts main and ignores bad entries") {
    using namespace swift::runtime;

    const auto valid = swift::linux::ParseModuleFeatureBindings(
            "main:ra_spill_evict=0,const_cse=1");
    REQUIRE(valid.warnings.empty());
    REQUIRE(valid.main.Get(FeatureId::ra_spill_evict) == false);
    REQUIRE(valid.main.Get(FeatureId::const_cse) == true);

    const auto mixed = swift::linux::ParseModuleFeatureBindings(
            "main:ra_spill_evict=2,not_a_feature=1,const_cse=0");
    REQUIRE(mixed.warnings.size() == 2);
    REQUIRE_FALSE(mixed.main.Get(FeatureId::ra_spill_evict).has_value());
    REQUIRE(mixed.main.Get(FeatureId::const_cse) == false);

    const auto unknown_role = swift::linux::ParseModuleFeatureBindings(
            "interp:ra_spill_evict=0");
    REQUIRE(unknown_role.warnings.size() == 1);
    REQUIRE(unknown_role.main.Empty());
    REQUIRE_FALSE(FeatureIdFromName("RA_SPILL_EVICT").has_value());
    REQUIRE(FeatureName(FeatureId::ra_spill_evict) == "ra_spill_evict");
}

TEST_CASE("mapped main-image range owns only its guest addresses") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;

    Config config{
            .loc_start = 0,
            .loc_end = 0x10000,
            .backend_isa = kArm64,
    };
    AddressSpace space{config};
    auto module_config = space.GetDefaultModule()->GetModuleConfig();
    module_config.feature_overrides.Set(FeatureId::ra_spill_evict, false);
    auto main_module = space.MapModule(0x2000, 0x5000, module_config);
    REQUIRE(space.GetModule(0x1fff) == space.GetDefaultModule());
    REQUIRE(space.GetModule(0x2000) == main_module);
    REQUIRE(space.GetModule(0x4fff) == main_module);
    REQUIRE(space.GetModule(0x5000) == space.GetDefaultModule());
    REQUIRE_FALSE(ResolveFeatureSet(main_module->GetModuleConfig()).ra_spill_evict);
    REQUIRE(ResolveFeatureSet(space.GetModule(0x1000)->GetModuleConfig()).ra_spill_evict ==
            GetSvmConfig().ra_spill_evict);

    const auto base_hash = ComputeConfigHash(
            config, space.GetDefaultModule()->GetModuleConfig());
    const std::array<swift::u64, 0> none{};
    REQUIRE(ComputeConfigHash(config,
                              space.GetDefaultModule()->GetModuleConfig(),
                              none) == base_hash);
    const std::array one{HashFeatureSet(ResolveFeatureSet(module_config))};
    REQUIRE(ComputeConfigHash(config,
                              space.GetDefaultModule()->GetModuleConfig(),
                              one) != base_hash);
}

TEST_CASE("SVM_FLAGS_REGS is A-class, default ON, and pins x12 out of XPOOL") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;

    REQUIRE(SvmConfig{}.flags_regs);
    REQUIRE(FeatureSet{}.jit_scratch_xpool);

    const GPRSMask empty_gprs{0};
    const FPRSMask empty_fprs{0};

    auto save = [](const char* name) {
        const char* old = GetRawSvmConfigEnvForTest(name);
        return std::pair<bool, std::string>{old != nullptr, old ? old : ""};
    };
    auto restore = [](const char* name, const std::pair<bool, std::string>& saved) {
        if (saved.first) {
            SetSvmConfigEnvForTest(name, saved.second.c_str(), 1);
        } else {
            UnsetSvmConfigEnvForTest(name);
        }
    };

    const auto old_flags = save("SVM_FLAGS_REGS");
    const auto old_latch = save("SVM_BACKEDGE_LATCH");
    const auto old_p1 = save("SVM_BACKEDGE_FLAGS");

    SetSvmConfigEnvForTest("SVM_FLAGS_REGS", "0", 1);
    REQUIRE_FALSE(FlagsRegsEnabled());
    {
        RegAlloc off{1, empty_gprs, empty_fprs, FeatureSet{}};
        REQUIRE_FALSE(off.GetGprs().Get(12));
    }
    UnsetSvmConfigEnvForTest("SVM_FLAGS_REGS");
    REQUIRE(FlagsRegsEnabled());
    const auto missing = ComputeEnvHash();
    SetSvmConfigEnvForTest("SVM_FLAGS_REGS", "0", 1);
    const auto disabled = ComputeEnvHash();
    SetSvmConfigEnvForTest("SVM_FLAGS_REGS", "1", 1);
    const auto enabled = ComputeEnvHash();

    REQUIRE(missing == enabled);
    REQUIRE(disabled != enabled);
    REQUIRE(FlagsRegsEnabled());
    {
        RegAlloc on{1, empty_gprs, empty_fprs, FeatureSet{}};
        REQUIRE(on.GetGprs().Get(12));
    }
    // Token ABI publishes on unit exits; it does not imply latch. P1 loses.
    UnsetSvmConfigEnvForTest("SVM_BACKEDGE_LATCH");
    UnsetSvmConfigEnvForTest("SVM_BACKEDGE_FLAGS");
    REQUIRE_FALSE(BackedgeLatchEnabled());
    REQUIRE_FALSE(BackedgeFlagsEnabled());
    SetSvmConfigEnvForTest("SVM_BACKEDGE_LATCH", "1", 1);
    SetSvmConfigEnvForTest("SVM_BACKEDGE_FLAGS", "1", 1);
    REQUIRE(BackedgeLatchEnabled());
    REQUIRE_FALSE(BackedgeFlagsEnabled());

    restore("SVM_BACKEDGE_FLAGS", old_p1);
    restore("SVM_BACKEDGE_LATCH", old_latch);
    restore("SVM_FLAGS_REGS", old_flags);
}

TEST_CASE("JIT cache environment hash separates absolute constant materialization") {
    const char* old = swift::runtime::GetRawSvmConfigEnvForTest("SVM_ABS_CONST_MAT");
    const bool had_old = old != nullptr;
    const std::string old_value = old ? old : "";

    swift::runtime::UnsetSvmConfigEnvForTest("SVM_ABS_CONST_MAT");
    const auto missing = swift::runtime::backend::ComputeEnvHash();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ABS_CONST_MAT", "0", 1);
    const auto disabled = swift::runtime::backend::ComputeEnvHash();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ABS_CONST_MAT", "1", 1);
    const auto enabled = swift::runtime::backend::ComputeEnvHash();

    if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_ABS_CONST_MAT", old_value.c_str(), 1);
    else swift::runtime::UnsetSvmConfigEnvForTest("SVM_ABS_CONST_MAT");

    REQUIRE(missing == enabled);
    REQUIRE(disabled != enabled);
    REQUIRE(missing != disabled);
}

TEST_CASE("config hash includes independent code-shape policies") {
    swift::runtime::Config base{};
    swift::runtime::Config a1{};
    swift::runtime::Config induction{};
    swift::runtime::Config region{};
    a1.mem_hostbase_fold = true;
    induction.induct_tie = true;
    region.region_edges = true;

    const auto base_hash = swift::runtime::backend::ComputeConfigHash(base);
    REQUIRE(swift::runtime::backend::ComputeConfigHash(a1) != base_hash);
    REQUIRE(swift::runtime::backend::ComputeConfigHash(induction) != base_hash);
    REQUIRE(swift::runtime::backend::ComputeConfigHash(region) != base_hash);
    REQUIRE(swift::runtime::backend::ComputeConfigHash(a1) !=
            swift::runtime::backend::ComputeConfigHash(induction));
}

TEST_CASE("config hash and default preserve software memory ordering") {
    using namespace swift::runtime;
    const char* old = GetRawSvmConfigEnvForTest("SVM_TSO_MODE");
    const bool had_old = old != nullptr;
    const std::string old_value = old ? old : "";
    UnsetSvmConfigEnvForTest("SVM_TSO_MODE");
    const auto mode = GetSvmConfig().tso_mode;
    const auto missing = backend::ComputeEnvHash();
    SetSvmConfigEnvForTest("SVM_TSO_MODE", "acqrel", 1);
    const auto explicit_mode = backend::ComputeEnvHash();
    SetSvmConfigEnvForTest("SVM_TSO_MODE", "relaxed", 1);
    const auto relaxed_mode = backend::ComputeEnvHash();
    if (had_old) SetSvmConfigEnvForTest("SVM_TSO_MODE", old_value.c_str(), 1);
    else UnsetSvmConfigEnvForTest("SVM_TSO_MODE");

    REQUIRE(mode == "acqrel");
    // String keys retain whether the caller supplied a value. Both forms
    // must remain separate from an explicitly weaker ordering mode.
    REQUIRE(explicit_mode != relaxed_mode);
    REQUIRE(missing != relaxed_mode);
    Config ordered{};
    Config relaxed{};
    Config hardware{};
    ordered.tso_mode = TsoMode::AcqRel;
    relaxed.tso_mode = TsoMode::Relaxed;
    hardware.tso_mode = TsoMode::Hardware;
    REQUIRE(backend::ComputeConfigHash(ordered) != backend::ComputeConfigHash(relaxed));
    REQUIRE(backend::ComputeConfigHash(ordered) != backend::ComputeConfigHash(hardware));
}
