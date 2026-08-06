#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "aarch64/disasm-aarch64.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/ir_meta.h"
#include "runtime/ir/opts/cfg_analysis_pass.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/reid_instr_pass.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"
#include "runtime/backend/mem_map.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/svm_config.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/frontend/x86/x87.h"
#include "compiler/slang/slang.h"
#include "assembler_riscv64.h"
#include "fmt/format.h"
#include "translator/linux/guest_memory.h"
#include "translator/linux/module_features.h"
#include "translator/x86/translator.h"

namespace {

using swift::runtime::FeatureSet;

swift::u64 ReadFPCRFromHostHelper() {
    return swift::runtime::backend::arm64::ReadNativeFPCR();
}

swift::u64 WriteMXCSRFromHostHelper(swift::u64 context, swift::u64 mxcsr) {
    auto* thread_context =
            reinterpret_cast<swift::x86::ThreadContext64*>(context);
    thread_context->mxcsr = static_cast<swift::u32>(mxcsr);
    return 0;
}

swift::u64 g_fpcr_transparent_helper_fpcr{};
swift::u64 g_fpcr_transparent_helper_fpsr{};

__attribute__((noinline)) swift::u64 ObserveFPEnvironmentFromTransparentHelper() {
#if defined(__aarch64__)
    asm volatile("mrs %0, fpcr" : "=r"(g_fpcr_transparent_helper_fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(g_fpcr_transparent_helper_fpsr));
#else
    g_fpcr_transparent_helper_fpcr = 0;
    g_fpcr_transparent_helper_fpsr = 0;
#endif
    return swift::u64{0x5a5a5a5a5a5a5a5a};
}

swift::u64 ReadNativeFPSR() {
#if defined(__aarch64__)
    swift::u64 value{};
    asm volatile("mrs %0, fpsr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

void WriteNativeFPSR(swift::u64 value) {
#if defined(__aarch64__)
    asm volatile("msr fpsr, %0" : : "r"(value) : "memory");
#else
    (void)value;
#endif
}

class ScopedNativeFPCR {
public:
    explicit ScopedNativeFPCR(swift::u64 value)
            : saved(swift::runtime::backend::arm64::ReadNativeFPCR()) {
        swift::runtime::backend::arm64::WriteNativeFPCR(value);
        installed = swift::runtime::backend::arm64::ReadNativeFPCR();
    }

    ~ScopedNativeFPCR() {
        swift::runtime::backend::arm64::WriteNativeFPCR(saved);
    }

    [[nodiscard]] swift::u64 Installed() const { return installed; }

private:
    swift::u64 saved{};
    swift::u64 installed{};
};

class ScopedNativeFPSR {
public:
    explicit ScopedNativeFPSR(swift::u64 value) : saved(ReadNativeFPSR()) {
        WriteNativeFPSR(value);
        installed = ReadNativeFPSR();
    }

    ~ScopedNativeFPSR() { WriteNativeFPSR(saved); }

    [[nodiscard]] swift::u64 Installed() const { return installed; }

private:
    swift::u64 saved{};
    swift::u64 installed{};
};

}  // namespace

TEST_CASE("Linux guest memory launch policy defaults to identity") {
    using swift::linux::SelectGuestMemoryLaunchPolicy;

    REQUIRE(SelectGuestMemoryLaunchPolicy(true, nullptr, nullptr).identity);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "0", nullptr).identity);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "OFF", nullptr).identity);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "off", nullptr).identity);
    REQUIRE(SelectGuestMemoryLaunchPolicy(true, "1", nullptr).identity);

    // Any explicit window-size request wins over the identity selector.
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, nullptr, "32").identity);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(true, "1", "36").identity);

    // macOS passes linux_host=false and ignores the identity selector.
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(false, nullptr, nullptr).identity);
    REQUIRE_FALSE(SelectGuestMemoryLaunchPolicy(false, "1", nullptr).identity);
}

TEST_CASE("identity image collision performs one bounded-bias fallback") {
    using swift::linux::GuestMemory;

    int identity_attempts = 0;
    int fallback_attempts = 0;
    REQUIRE(GuestMemory::TryIdentityWithFallback(
            [&] {
                ++identity_attempts;
                return false;
            },
            [&] {
                ++fallback_attempts;
                return true;
            }));
    REQUIRE(identity_attempts == 1);
    REQUIRE(fallback_attempts == 1);

    identity_attempts = 0;
    fallback_attempts = 0;
    REQUIRE(GuestMemory::TryIdentityWithFallback(
            [&] {
                ++identity_attempts;
                return true;
            },
            [&] {
                ++fallback_attempts;
                return false;
            }));
    REQUIRE(identity_attempts == 1);
    REQUIRE(fallback_attempts == 0);
}

TEST_CASE("identity guest unmap preserves untracked host mappings") {
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
    memory.EnableIdentityMode();
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

TEST_CASE("hot coalesce probe classifies static opportunities") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;

    REQUIRE(PerfStats2::kGetenvNames.size() == 133);
    REQUIRE(PerfStats2::kGetenvNames.size() == kSvmConfigFieldCount);
    REQUIRE(std::string_view(PerfStats2::kGetenvNames.front()) ==
            "SVM_MEM_IDENTITY");
    REQUIRE(std::string_view(PerfStats2::kGetenvNames.back()) ==
            "SWIFT_FUZZ_TRACE");
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

TEST_CASE("helper FP effects default conservative and compose with ABI metadata") {
    using namespace swift::runtime::ir;

    const auto address = DataClass{Imm{swift::u64{0x1234}}};
    const Lambda ordinary{address};
    REQUIRE(ordinary.GetHostFpEffect() == HostFpEffect::MayTouch);
    REQUIRE(ordinary.GetHelperABI() == HelperABI::NormalAAPCS);
    REQUIRE(ordinary.GetUniformEffectId() == UniformEffectId::Unknown);

    const Lambda combined{
            address,
            HelperCallTraits{
                    .uniform = UniformEffectId::None,
                    .abi = HelperABI::PreserveAllLeaf,
                    .host_fp = HostFpEffect::FPCRTransparent,
            }};
    REQUIRE(combined.GetImm().Get() == swift::u64{0x1234});
    REQUIRE(combined.GetHostFpEffect() == HostFpEffect::FPCRTransparent);
    REQUIRE(combined.GetHelperABI() == HelperABI::PreserveAllLeaf);
    REQUIRE(combined.GetUniformEffectId() == UniformEffectId::None);
}

TEST_CASE("config hash includes programmatic effective AFP policy") {
    swift::runtime::Config off{};
    swift::runtime::Config on{};
    on.sse_afp_nan = true;

    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) !=
            swift::runtime::backend::ComputeConfigHash(on));
    on.sse_afp_nan = false;
    REQUIRE(swift::runtime::backend::ComputeConfigHash(off) ==
            swift::runtime::backend::ComputeConfigHash(on));
}

TEST_CASE("FeatureSet snapshots every B-class field and applies sparse overrides") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;

    STATIC_REQUIRE(kFeatureCount == 47);
    const auto& svm = GetSvmConfig();
    const auto snapshot = svm.GetFeatureSet();
#define CHECK_FEATURE_COPY(field, default_value) REQUIRE(snapshot.field == svm.field);
    SVM_FEATURE_FIELDS(CHECK_FEATURE_COPY)
#undef CHECK_FEATURE_COPY

    ModuleConfig empty{};
    const auto resolved_empty = ResolveFeatureSet(empty);
#define CHECK_EMPTY_RESOLVE(field, default_value) \
    REQUIRE(resolved_empty.field == snapshot.field);
    SVM_FEATURE_FIELDS(CHECK_EMPTY_RESOLVE)
#undef CHECK_EMPTY_RESOLVE

    ModuleConfig overridden{};
    overridden.feature_overrides.Set(FeatureId::const_cse, !snapshot.const_cse);
    const auto resolved_override = ResolveFeatureSet(overridden);
    REQUIRE(resolved_override.const_cse != snapshot.const_cse);
    REQUIRE(resolved_override.uniform_dse == snapshot.uniform_dse);

    Config config{};
    REQUIRE(ComputeConfigHash(config, empty) == ComputeConfigHash(config));
    REQUIRE(ComputeConfigHash(config, overridden) != ComputeConfigHash(config));
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

    REQUIRE(missing == disabled);
    REQUIRE(disabled != enabled);
    REQUIRE(missing != enabled);
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

TEST_CASE("Test compiler") {
    using namespace swift::slang;
    Context context{};
    CompileFile("/Users/swift/CLionProjects/SwiftVM/source/tests/test.slang", context);
}

TEST_CASE("Test runtime ir") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1, false, FeatureSet{}};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local_arg1{
            .id = 0,
            .type = ValueType::U32,
    };
    Local local_arg2 {
            .id = 1,
            .type = ValueType::U32,
    };
    Local local_arg3 {
            .id = 2,
            .type = ValueType::U32,
    };
    function->DefineLocal(local_arg1);
    function->DefineLocal(local_arg2);
    function->DefineLocal(local_arg3);
    auto const1 = function->LoadImm(Imm(UINT32_MAX));
    auto const2 = function->LoadImm(Imm(UINT32_MAX-1));
    function->StoreLocal(local_arg1, const1);
    auto local1 = function->LoadLocal(local_arg1);
    function->StoreLocal(local_arg2, local1);
    auto local2 = function->LoadLocal(local_arg2);
    auto [else_, then_] = hir_builder.If(terminal::If{local2, terminal::LinkBlock{1}, terminal::LinkBlock{2}});
    hir_builder.SetCurBlock(then_);
    function->StoreLocal(local_arg3, const1);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(else_);
    function->StoreLocal(local_arg3, const2);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(3);
    function->StoreUniform(Uniform{0, ValueType::U32}, function->LoadLocal(local_arg3));
    Params params{};
    params.Push(local1);
    params.Push(local2);
    hir_builder.CallDynamic(Lambda(Imm(uint64_t(1))), params);

    hir_builder.Return();
    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
    RegAlloc reg_alloc{function->MaxInstrCount(), GPRSMask{0}, FPRSMask{0},
                       FeatureSet{}};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc, FeatureSet{});

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);
}

TEST_CASE("Test runtime ir cfg") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1, false, FeatureSet{}};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local_arg1{
            .id = 0,
            .type = ValueType::U32,
    };
    Local local_arg2{
            .id = 1,
            .type = ValueType::U32,
    };
    Local local_arg3{
            .id = 2,
            .type = ValueType::U32,
    };
    function->DefineLocal(local_arg1);
    function->DefineLocal(local_arg2);
    function->DefineLocal(local_arg3);
    auto const1 = function->LoadImm(Imm(UINT32_MAX));
    auto const2 = function->LoadImm(Imm(UINT32_MAX-1));
    function->StoreLocal(local_arg1, const1);
    auto local1 = function->LoadLocal(local_arg1);
    function->StoreLocal(local_arg2, local1);
    auto local2 = function->LoadLocal(local_arg2);
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{2}));
    function->StoreLocal(local_arg3, const1);
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{3}));
    function->StoreLocal(local_arg3, const2);
    function->StoreUniform(Uniform{0, ValueType::U32}, function->LoadLocal(local_arg3));
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{4}));
    Params params{};
    params.Push(local1);
    params.Push(local2);
    hir_builder.CallDynamic(Lambda(Imm(uint64_t(1))), params);

    hir_builder.Return();
    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
#define ARM64_X_REGS_MASK 0b1111111111111111111
    swift::runtime::backend::GPRSMask gprs{ARM64_X_REGS_MASK};
    swift::runtime::backend::FPRSMask fprs{ARM64_X_REGS_MASK};
    RegAlloc reg_alloc{0x100, gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc, FeatureSet{});

    assert(local2.Defined());

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);

}

TEST_CASE("Test runtime ir loop") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1, false, FeatureSet{}};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local1{
            .id = 0,
            .type = ValueType::U32,
    };
    auto value = function->LoadLocal(local1);
    auto [else_, then_] = hir_builder.If(terminal::If{value, terminal::LinkBlock{1}, terminal::LinkBlock{2}});
    else_->StoreLocal(local1, else_->LoadImm(Imm(UINT32_MAX)));
    then_->StoreLocal(local1, else_->LoadImm(Imm(UINT32_MAX)));
    hir_builder.SetCurBlock(else_);
    hir_builder.LinkBlock(terminal::LinkBlock{0});
    hir_builder.SetCurBlock(then_);
    hir_builder.Return();

    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
}

TEST_CASE("CFG analysis terminates and computes dominators for an irreducible loop") {
    using namespace swift::runtime::ir;

    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);

    HIRBuilder hir_builder{1, false, FeatureSet{}};
    auto* function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    auto condition = function->LoadImm<BOOL>(Imm{1u});

    // Minimal trigger (five real blocks plus the synthetic entry):
    //
    //            +----------------> exit
    //            |                   ^
    //   root --> left --> loop -------+
    //     |               |  ^
    //     +-----> right ---+  |
    //              ^          |
    //              +----------+
    //
    // `right <-> loop` is an irreducible cycle with entries from root and
    // left. The old one-pass dominator walk first made left dominate exit,
    // then revised loop's dominator after seeing the second cycle entry without
    // propagating that revision to exit. Dominance-frontier construction then
    // chased the stale chain to entry, whose dominator is itself, forever.
    auto [left, right] =
            hir_builder.If(terminal::If{condition,
                                       terminal::LinkBlock{2},
                                       terminal::LinkBlock{1}});
    hir_builder.SetCurBlock(left);
    auto [loop, exit] =
            hir_builder.If(terminal::If{condition,
                                       terminal::LinkBlock{4},
                                       terminal::LinkBlock{3}});
    hir_builder.SetCurBlock(right);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(loop);
    hir_builder.If(terminal::If{condition,
                               terminal::LinkBlock{4},
                               terminal::LinkBlock{2}});
    hir_builder.SetCurBlock(exit);
    hir_builder.Return();

    // Run the analysis in a subprocess so a future termination regression
    // fails this case after two seconds instead of wedging the whole test job.
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        alarm(2);
        CFGAnalysisPass::Run(function);
        alarm(0);

        auto* entry = function->GetEntryBlock();
        const bool dominators_are_correct =
                entry->GetDominator() == entry &&
                function->GetHIRBlocks()[1]->GetDominator() == entry &&
                left->GetDominator() == function->GetHIRBlocks()[1] &&
                right->GetDominator() == function->GetHIRBlocks()[1] &&
                loop->GetDominator() == function->GetHIRBlocks()[1] &&
                exit->GetDominator() == function->GetHIRBlocks()[1];
        _exit(dominators_are_correct ? 0 : 1);
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("Test riscv64 asm") {
    using namespace swift;
    riscv64::Riscv64Label label{};
    riscv64::ArenaAllocator allocator{};
    riscv64::Riscv64Assembler assembler{&allocator};
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bind(&label);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bne(riscv64::A1, riscv64::A2, &label);
    assembler.FinalizeCode();
}

TEST_CASE("Test runtime") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    swift::runtime::Config config {
            .loc_start = 0,
            // Must be a realistic guest range, matching the arm64 translator
            // (translator/arm64/translator.cpp). Module's AddressHashMap
            // reserves one pointer per 1 MB of [loc_start, loc_end): 2^48
            // costs a 2 MB reservation, while UINT64_MAX would ask mmap for
            // 128 TB and abort in AllocateMemoryPages.
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    // Module::Push takes ownership: it calls IntrusivePtrAddRef, and the
    // matching release eventually runs Block::operator delete -> SlabObject::
    // TryFree, which hands a non-slab pointer to libc free(). A stack-allocated
    // Block therefore ends the test with free() on a stack address (SIGABRT).
    // Heap-allocate, matching how TranslateIR feeds Push in runtime.cpp.
    auto* block1 = new Block(0, Location{1});
    auto* block2 = new Block(1, Location{2});
    REQUIRE(module->Push(block1));
    REQUIRE(module->Push(block2));
}

TEST_CASE("Runtime preserves an interrupt between Run calls") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    Runtime runtime{&address_space};

    bool initial_miss = true;
    bool interrupt_seen = true;
    bool resumed_miss = true;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        initial_miss &= runtime.Run() == HaltReason::CodeMiss;
        // Deliberately publish after Run has returned and before the next Run:
        // this is the W64 empty-loop window, repeated without changing the
        // suite's assertion count.
        runtime.SignalInterrupt();
        interrupt_seen &= runtime.Run() == HaltReason::Signal;
        runtime.ClearInterrupt();
        resumed_miss &= runtime.Run() == HaltReason::CodeMiss;
    }
    REQUIRE(initial_miss);
    REQUIRE(interrupt_seen);
    REQUIRE(resumed_miss);
}

TEST_CASE("Test block ir print") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Block block{0, Location{0x1000}};
    auto imm32 = block.LoadImm(Imm{8u}).SetType(ValueType::U32);
    auto imm8 = block.LoadImm<BOOL>(Imm{8u}).SetType(ValueType::U8);
    block.StoreUniform(Uniform{32, ValueType::U32}, imm8);
    Params params{};
    params.Push(imm8);
    params.Push(imm8);
    block.CallDynamic(Lambda(Imm(uint64_t(1))), params);
    block.SaveFlags(imm8, Flags{Flags::NZCV});
    block.SetTerminal(terminal::If(terminal::If{imm8, terminal::LinkBlock{0x1000}, terminal::LinkBlock{0x2000}}));
    std::cout << block.ToString() << std::endl;
}

TEST_CASE("Uniform elimination does not propagate conditional stores past labels") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};

    Block conditional{0, Location{0x1000}};
    auto condition = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip_store = conditional.NotGoto(condition);
    auto stored = conditional.LoadImm(Imm{0u}).SetType(ValueType::U8);
    conditional.StoreUniform(Uniform{32, ValueType::U8}, stored);
    conditional.BindLabel(skip_store);
    auto merged_load = conditional.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&conditional, info, FeatureSet{});
    REQUIRE(merged_load.Def()->GetOp() == OpCode::LoadUniform);

    Block straight_line{1, Location{0x2000}};
    auto straight_value = straight_line.LoadImm(Imm{0u}).SetType(ValueType::U8);
    straight_line.StoreUniform(Uniform{32, ValueType::U8}, straight_value);
    auto straight_load = straight_line.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&straight_line, info, FeatureSet{});
    REQUIRE(straight_load.Def()->GetOp() == OpCode::BitExtract);

    // A value established before the branch dominates both successors. W14's
    // path merge keeps it when the guarded region leaves that uniform byte
    // untouched, while SVM_UNIFORM_PATH_FWD=0 restores the old label barrier.
    Block dominated{2, Location{0x3000}};
    auto dominating_value = dominated.LoadImm(Imm{7u}).SetType(ValueType::U8);
    dominated.StoreUniform(Uniform{32, ValueType::U8}, dominating_value);
    auto dominated_cond = dominated.LoadImm<BOOL>(Imm{1u});
    auto dominated_skip = dominated.NotGoto(dominated_cond);
    dominated.LoadImm(Imm{99u});
    dominated.BindLabel(dominated_skip);
    auto dominated_load = dominated.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&dominated, info, FeatureSet{});
    const bool path_forward_off =
            !swift::runtime::GetSvmConfig().uniform_path_fwd;
    REQUIRE(dominated_load.Def()->GetOp() ==
            (path_forward_off ? OpCode::LoadUniform : OpCode::BitExtract));

    // A write on only the fallthrough edge must still prevent forwarding.
    Block differing{3, Location{0x4000}};
    auto before = differing.LoadImm(Imm{7u}).SetType(ValueType::U8);
    differing.StoreUniform(Uniform{32, ValueType::U8}, before);
    auto differing_cond = differing.LoadImm<BOOL>(Imm{1u});
    auto differing_skip = differing.NotGoto(differing_cond);
    auto guarded = differing.LoadImm(Imm{9u}).SetType(ValueType::U8);
    differing.StoreUniform(Uniform{32, ValueType::U8}, guarded);
    differing.BindLabel(differing_skip);
    auto differing_load = differing.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&differing, info, FeatureSet{});
    REQUIRE(differing_load.Def()->GetOp() == OpCode::LoadUniform);

    // The reverse walk uses the same path intersection. A guarded write is
    // dead when a later store overwrites it after the merge on both paths.
    Block guarded_dead{4, Location{0x5000}};
    auto dead_cond = guarded_dead.LoadImm<BOOL>(Imm{1u});
    auto dead_skip = guarded_dead.NotGoto(dead_cond);
    auto dead_value = guarded_dead.LoadImm(Imm{3u}).SetType(ValueType::U8);
    guarded_dead.StoreUniform(Uniform{32, ValueType::U8}, dead_value);
    guarded_dead.BindLabel(dead_skip);
    auto final_value = guarded_dead.LoadImm(Imm{4u}).SetType(ValueType::U8);
    guarded_dead.StoreUniform(Uniform{32, ValueType::U8}, final_value);

    UniformEliminationPass::Run(&guarded_dead, info, FeatureSet{});
    size_t guarded_store_count{};
    for (const auto& inst : guarded_dead.GetInstList()) {
        guarded_store_count += inst.GetOp() == OpCode::StoreUniform;
    }
    REQUIRE(guarded_store_count == (path_forward_off ? 2 : 1));
}

TEST_CASE("Uniform range mode keeps mapped GPR, XMM, and ordinary byte facts local") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 96};
    auto map_gpr = [&](std::uint32_t begin, std::uint16_t host_id) {
        UniformRegister reg{.uniform = Uniform{begin, ValueType::U64}};
        reg.host_reg.gpr = HostGPR{host_id};
        reg.host_reg.is_fpr = false;
        info.uniform_regs_map.Map(begin, begin + sizeof(std::uint64_t), reg);
    };
    map_gpr(16, 20);
    map_gpr(32, 21);
    info.xmm_uniform_ranges.push_back({48, 64});

    const bool range_on = swift::runtime::GetSvmConfig().ir_uniform_range;

    Block block{0, Location{0x6000}};
    auto ordinary = block.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{0, ValueType::U64}, ordinary);
    auto other_gpr = block.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{32, ValueType::U64}, other_gpr);
    auto xmm = block.LoadImm(Imm{0u}).SetType(ValueType::V128);
    block.StoreUniform(Uniform{48, ValueType::V128}, xmm);
    auto target_gpr = block.LoadImm(Imm{std::uint64_t(0x3333333333333333ull)}).SetType(ValueType::U64);
    block.StoreUniform(Uniform{16, ValueType::U64}, target_gpr);
    auto target_patch =
            block.LoadImm(Imm{std::uint16_t{0x4444}}).SetType(ValueType::U16);
    block.StoreUniform(Uniform{18, ValueType::U16}, target_patch);

    auto untouched_target = block.LoadUniform(Uniform{16, ValueType::U16});
    auto written_target = block.LoadUniform(Uniform{18, ValueType::U16});
    auto preserved_gpr = block.LoadUniform(Uniform{32, ValueType::U64});
    auto preserved_xmm = block.LoadUniform(Uniform{48, ValueType::V128});
    auto preserved_ordinary = block.LoadUniform(Uniform{0, ValueType::U64});

    UniformEliminationPass::Run(&block, info, FeatureSet{});
    REQUIRE(untouched_target.Def()->GetOp() ==
            (range_on ? OpCode::BitExtract : OpCode::GetHostGPR));
    REQUIRE(written_target.Def()->GetOp() ==
            (range_on ? OpCode::BitExtract : OpCode::GetHostGPR));
    REQUIRE(preserved_gpr.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::GetHostGPR));
    REQUIRE(preserved_xmm.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
    REQUIRE(preserved_ordinary.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
}

TEST_CASE("Uniform helper effect sets preserve only unaffected facts") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 32};
    static constexpr std::array touched_ranges{
            UniformEffectRange{0, sizeof(std::uint64_t)},
    };
    static constexpr UniformEffectSet touched_effects{
            touched_ranges.data(), touched_ranges.size()};
    const auto touched_id = RegisterUniformEffectSet(&touched_effects);
    const bool range_on = swift::runtime::GetSvmConfig().ir_uniform_range;

    auto seed = [](Block& block) {
        auto low = block.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
        auto high = block.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
        block.StoreUniform(Uniform{0, ValueType::U64}, low);
        block.StoreUniform(Uniform{16, ValueType::U64}, high);
    };

    Block pure{0, Location{0x7000}};
    seed(pure);
    pure.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, UniformEffectId::None});
    auto pure_low = pure.LoadUniform(Uniform{0, ValueType::U64});
    auto pure_high = pure.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&pure, info, FeatureSet{});
    REQUIRE(pure_low.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));
    REQUIRE(pure_high.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));

    Block ranged{1, Location{0x8000}};
    seed(ranged);
    ranged.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, touched_id});
    auto ranged_low = ranged.LoadUniform(Uniform{0, ValueType::U64});
    auto ranged_high = ranged.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&ranged, info, FeatureSet{});
    REQUIRE(ranged_low.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(ranged_high.Def()->GetOp() ==
            (range_on ? OpCode::BitCast : OpCode::LoadUniform));

    Block unknown{2, Location{0x9000}};
    seed(unknown);
    unknown.CallLambda(Lambda{Imm{std::uint64_t(1ull)}});
    auto unknown_low = unknown.LoadUniform(Uniform{0, ValueType::U64});
    auto unknown_high = unknown.LoadUniform(Uniform{16, ValueType::U64});
    UniformEliminationPass::Run(&unknown, info, FeatureSet{});
    REQUIRE(unknown_low.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(unknown_high.Def()->GetOp() == OpCode::LoadUniform);

    Block dead_store{3, Location{0xa000}};
    auto old_value =
            dead_store.LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
    auto new_value =
            dead_store.LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
    dead_store.StoreUniform(Uniform{0, ValueType::U64}, old_value);
    dead_store.CallLambda(Lambda{DataClass{Imm{std::uint64_t(1ull)}}, UniformEffectId::None});
    dead_store.StoreUniform(Uniform{0, ValueType::U64}, new_value);
    UniformEliminationPass::Run(&dead_store, info, FeatureSet{});
    size_t stores{};
    for (const auto& inst : dead_store.GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
    }
    // Effect sets describe helper writes for forward fact invalidation. They
    // do not claim the helper cannot read uniform state, so reverse DSE keeps
    // every helper as an observation barrier.
    REQUIRE(stores == 2);
}

TEST_CASE("XMM uniform forwarding covers V128 and scalar views") {
    using namespace swift::runtime::ir;

    // The pass is runtime-generic, so describe a synthetic XMM slot exactly
    // as the x86 frontend does: both the full V128 access and the two U64
    // architectural views live in one byte range.
    UniformInfo info{.uniform_size = 64};
    info.xmm_uniform_ranges.push_back({16, 32});
    const auto& svm_config = swift::runtime::GetSvmConfig();
    const bool xmm_forward_off = !svm_config.xmm_uniform_fwd;
    const bool xmm_ssa_fwd2_off = !svm_config.xmm_ssa_fwd2;
    const bool xmm_narrow_fwd_off = !svm_config.xmm_narrow_fwd;

    Block straight{0, Location{0x1000}};
    auto vector_value = straight.LoadImm(Imm{0u}).SetType(ValueType::V128);
    straight.StoreUniform(Uniform{16, ValueType::V128}, vector_value);
    auto vector_load = straight.LoadUniform(Uniform{16, ValueType::V128});
    auto narrow64_load = straight.LoadUniform(Uniform{16, ValueType::V64});
    auto narrow32_load = straight.LoadUniform(Uniform{16, ValueType::V32});
    auto scalar_value = straight.LoadImm(Imm{std::uint64_t(0x1122334455667788ull)}).SetType(ValueType::U64);
    straight.StoreUniform(Uniform{24, ValueType::U64}, scalar_value);
    auto scalar_load = straight.LoadUniform(Uniform{24, ValueType::U64});

    UniformEliminationPass::Run(&straight, info, FeatureSet{});
    REQUIRE(vector_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));
    const auto narrow_expected =
            (!xmm_forward_off && !xmm_narrow_fwd_off) ? OpCode::BitCast
                                                      : OpCode::LoadUniform;
    REQUIRE(narrow64_load.Def()->GetOp() == narrow_expected);
    REQUIRE(narrow64_load.Type() == ValueType::V64);
    REQUIRE(narrow32_load.Def()->GetOp() == narrow_expected);
    REQUIRE(narrow32_load.Type() == ValueType::V32);
    REQUIRE(scalar_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // A fact established before the guarded region dominates both paths. The
    // same byte-for-byte intersection used by W14's GPR path must preserve it
    // for a V128 value too.
    Block guarded{1, Location{0x2000}};
    auto guarded_value = guarded.LoadImm(Imm{1u}).SetType(ValueType::V128);
    guarded.StoreUniform(Uniform{16, ValueType::V128}, guarded_value);
    auto condition = guarded.LoadImm<BOOL>(Imm{1u});
    auto skip = guarded.NotGoto(condition);
    guarded.LoadImm(Imm{7u});
    guarded.BindLabel(skip);
    auto merged_load = guarded.LoadUniform(Uniform{16, ValueType::V128});
    UniformEliminationPass::Run(&guarded, info, FeatureSet{});
    REQUIRE(merged_load.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // With no read between them, the first whole-XMM store is dead only while
    // this forwarding family is enabled.  The off mode is intentionally a
    // precise rollback: neither vector forwarding nor its dead-store sweep
    // touches the XMM range.
    Block dead_store{2, Location{0x3000}};
    auto old_value = dead_store.LoadImm(Imm{2u}).SetType(ValueType::V128);
    auto new_value = dead_store.LoadImm(Imm{3u}).SetType(ValueType::V128);
    dead_store.StoreUniform(Uniform{16, ValueType::V128}, old_value);
    dead_store.StoreUniform(Uniform{16, ValueType::V128}, new_value);
    UniformEliminationPass::Run(&dead_store, info, FeatureSet{});
    size_t stores{};
    for (const auto& inst : dead_store.GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
    }
    REQUIRE(stores == (xmm_forward_off ? 2 : 1));

    // Phase 2 seeds the byte table from a materialized XMM load as well as a
    // store. This is the hot AES/GHASH shape: a live-in key/state register is
    // read repeatedly without an intervening architectural write.
    Block repeated_load{3, Location{0x4000}};
    auto first_vector_load = repeated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto low_lane_load = repeated_load.LoadUniform(Uniform{16, ValueType::V64});
    auto second_vector_load = repeated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto first_lane_load = repeated_load.LoadUniform(Uniform{24, ValueType::U64});
    auto second_lane_load = repeated_load.LoadUniform(Uniform{24, ValueType::U64});
    UniformEliminationPass::Run(&repeated_load, info, FeatureSet{});
    REQUIRE(first_vector_load.Def()->GetOp() == OpCode::LoadUniform);
    REQUIRE(low_lane_load.Def()->GetOp() ==
            (!xmm_forward_off && !xmm_ssa_fwd2_off && !xmm_narrow_fwd_off
                     ? OpCode::BitCast
                     : OpCode::LoadUniform));
    REQUIRE(first_lane_load.Def()->GetOp() == OpCode::LoadUniform);
    const auto repeated_vector_expected =
            (!xmm_forward_off && !xmm_ssa_fwd2_off && !xmm_narrow_fwd_off)
                    ? OpCode::BitCast
                    : OpCode::LoadUniform;
    const auto repeated_lane_expected =
            (!xmm_forward_off && !xmm_ssa_fwd2_off) ? OpCode::BitCast
                                                    : OpCode::LoadUniform;
    REQUIRE(second_vector_load.Def()->GetOp() == repeated_vector_expected);
    REQUIRE(second_lane_load.Def()->GetOp() == repeated_lane_expected);

    // An intervening write must still replace the load fact byte-for-byte.
    Block invalidated_load{4, Location{0x5000}};
    invalidated_load.LoadUniform(Uniform{16, ValueType::V128});
    auto replacement = invalidated_load.LoadImm(Imm{4u}).SetType(ValueType::V128);
    invalidated_load.StoreUniform(Uniform{16, ValueType::V128}, replacement);
    auto after_store = invalidated_load.LoadUniform(Uniform{16, ValueType::V128});
    UniformEliminationPass::Run(&invalidated_load, info, FeatureSet{});
    REQUIRE(after_store.Def()->GetOp() ==
            (xmm_forward_off ? OpCode::LoadUniform : OpCode::BitCast));

    // PIN_EXT performs its generic DSE before mapped GPR stores become
    // SetHostGPR. A narrow XMM load is still materialized at that point, so a
    // second XMM-only sweep must collect the old store after the load folds;
    // the disjoint mapped GPR write is not an XMM observation boundary.
    UniformInfo pinned_info{.uniform_size = 128};
    auto map_pin = [&](std::uint32_t offset, std::uint16_t host_id) {
        UniformRegister pin{.uniform = Uniform{offset, ValueType::U64}};
        pin.host_reg.gpr = HostGPR{host_id};
        pin.host_reg.is_fpr = false;
        pinned_info.uniform_regs_map.Map(offset, offset + 8, pin);
        pinned_info.uni_gprs.Mark(host_id);
    };
    map_pin(0, 22);
    map_pin(8, 23);
    map_pin(16, 29);
    pinned_info.xmm_uniform_ranges.push_back({64, 80});
    Block pinned{6, Location{0x7000}};
    auto old_xmm = pinned.LoadImm(Imm{5u}).SetType(ValueType::V128);
    pinned.StoreUniform(Uniform{64, ValueType::V128}, old_xmm);
    auto pinned_lane = pinned.LoadUniform(Uniform{64, ValueType::V64});
    auto pin_value = pinned.LoadImm(Imm{std::uint64_t{7}}).SetType(ValueType::U64);
    pinned.StoreUniform(Uniform{0, ValueType::U64}, pin_value);
    auto new_xmm = pinned.LoadImm(Imm{6u}).SetType(ValueType::V128);
    pinned.StoreUniform(Uniform{64, ValueType::V128}, new_xmm);
    UniformEliminationPass::Run(&pinned, pinned_info, FeatureSet{});
    REQUIRE(pinned_lane.Def()->GetOp() ==
            (xmm_forward_off || xmm_narrow_fwd_off ? OpCode::LoadUniform
                                                   : OpCode::BitCast));
    size_t pinned_xmm_stores{};
    for (const auto& inst : pinned.GetInstList()) {
        if (inst.GetOp() != OpCode::StoreUniform) continue;
        const auto uniform = inst.GetArg<Uniform>(0);
        pinned_xmm_stores += uniform.GetOffset() == 64;
    }
    REQUIRE(pinned_xmm_stores ==
            (xmm_forward_off || xmm_narrow_fwd_off ? 2 : 1));
}

TEST_CASE("Uniform fast path is instruction-identical to the legacy pass") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};
    auto make_uniform_block = [] {
        auto block = std::make_unique<Block>(0, Location{0x3000});

        // Full overwrite: the first store is dead.
        auto first = block->LoadImm(Imm{std::uint64_t(0x1111111111111111ull)}).SetType(ValueType::U64);
        auto latest = block->LoadImm(Imm{std::uint64_t(0x2222222222222222ull)}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{0, ValueType::U64}, first);
        block->StoreUniform(Uniform{0, ValueType::U64}, latest);

        // Narrow forwarding must stay a BitExtract with the same source/type.
        block->LoadUniform(Uniform{2, ValueType::U16});

        // The opaque call invalidates forwarding facts. The following load
        // must remain a LoadUniform in both implementations.
        Params params{};
        block->CallDynamic(Lambda(Imm{1u}), params);
        block->LoadUniform(Uniform{2, ValueType::U16});

        // Two overlapping later stores jointly cover the earlier U64 store.
        // This pins byte-range aliasing rather than just exact-offset DSE.
        auto wide = block->LoadImm(Imm{std::uint64_t(0x3333333333333333ull)}).SetType(ValueType::U64);
        auto low = block->LoadImm(Imm{0x44444444u}).SetType(ValueType::U32);
        auto high = block->LoadImm(Imm{0x55555555u}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{16, ValueType::U64}, wide);
        block->StoreUniform(Uniform{16, ValueType::U32}, low);
        block->StoreUniform(Uniform{20, ValueType::U32}, high);
        // Spans two different stored values, so it must not forward.
        block->LoadUniform(Uniform{18, ValueType::U32});
        return block;
    };

    auto legacy = make_uniform_block();
    auto fast = make_uniform_block();
    UniformEliminationPass::Run(legacy.get(), info, false, FeatureSet{});
    UniformEliminationPass::Run(fast.get(), info, true, FeatureSet{});

    // ToString includes every opcode, result type, argument and defining id;
    // equality is therefore a per-instruction IR comparison, not just counts.
    REQUIRE(fast->ToString() == legacy->ToString());
    size_t stores = 0;
    size_t loads = 0;
    size_t extracts = 0;
    for (const auto& inst : fast->GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
        loads += inst.GetOp() == OpCode::LoadUniform;
        extracts += inst.GetOp() == OpCode::BitExtract;
    }
    REQUIRE(stores == 3);
    REQUIRE(loads == 2);
    REQUIRE(extracts == 1);

    // No-uniform early return must preserve even an otherwise trivial block.
    Block legacy_plain{1, Location{0x4000}};
    Block fast_plain{1, Location{0x4000}};
    legacy_plain.LoadImm(Imm{7u}).SetType(ValueType::U32);
    fast_plain.LoadImm(Imm{7u}).SetType(ValueType::U32);
    UniformEliminationPass::Run(&legacy_plain, info, false, FeatureSet{});
    UniformEliminationPass::Run(&fast_plain, info, true, FeatureSet{});
    REQUIRE(fast_plain.ToString() == legacy_plain.ToString());
}

namespace {

struct IRBuildFixture {
    explicit IRBuildFixture(bool fast)
            : builder(1, true, fast) {
        using namespace swift::runtime::ir;

        function = builder.AppendFunction(Location{0x1000}, Location{0x1200});
        const Local local{.id = 7, .type = ValueType::U64};
        function->DefineLocal(local);

        auto imm = function->LoadImm(Imm{std::uint64_t(0x1122334455667788ull)})
                           .SetType(ValueType::U64);
        first_inst = imm.Def();
        auto uniform =
                function->LoadUniform(Uniform{24, ValueType::U64}).SetType(ValueType::U64);
        auto operand =
                function->Add(imm, Operand{uniform, imm, OperandLsl}).SetType(ValueType::U64);
        auto selected =
                function->CondSelect(Cond::EQ, operand, uniform).SetType(ValueType::U64);
        function->SaveFlags(selected, Flags::All);
        function->StoreLocal(local, selected);
        auto local_value = function->LoadLocal(local).SetType(ValueType::U64);

        // Lambda-as-Value plus three ordinary Value slots.
        [[maybe_unused]] auto lambda_result =
                function->CallLambda(Lambda{selected}, imm, uniform, operand)
                        .SetType(ValueType::U64);

        // Params deliberately repeats `imm`: order and duplicates are part of
        // the HIR use-list contract, not a set.
        Params params{};
        params.Push(imm);
        params.Push(Imm{0xa5a5u});
        params.Push(uniform);
        params.Push(imm);
        [[maybe_unused]] auto dynamic_result =
                function->CallDynamic(Lambda{Imm{std::uint64_t(0x1234ull)}}, params)
                        .SetType(ValueType::U64);
        function->StoreUniform(Uniform{40, ValueType::U64}, local_value);

        // More than one 64 KiB Inst arena chunk even at the minimum possible
        // slot size. All instructions stay live until both fixtures have been
        // compared, pinning pointer stability across growth.
        for (unsigned i = 0; i < 1536; ++i) {
            function->Nop();
        }

        auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x1100}});
        builder.SetCurBlock(next);
        auto tail = function->Add(selected, Operand{imm}).SetType(ValueType::U64);
        function->StoreUniform(Uniform{48, ValueType::U64}, tail);
        function->EndBlock(terminal::ReturnToDispatch{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
    }

    swift::runtime::ir::HIRBuilder builder{1, false, FeatureSet{}};
    swift::runtime::ir::HIRFunction* function{};
    swift::runtime::ir::Inst* first_inst{};
};

std::string IRSnapshotArg(swift::runtime::ir::Arg& arg) {
    using namespace swift::runtime::ir;
    switch (arg.GetType()) {
        case ArgType::Void:
            return "void";
        case ArgType::Value: {
            const auto value = arg.Get<Value>();
            return fmt::format("value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
        }
        case ArgType::Imm: {
            const auto imm = arg.Get<Imm>();
            return fmt::format("imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Cond:
            return fmt::format("cond:{}", static_cast<unsigned>(arg.Get<Cond>()));
        case ArgType::Flags:
            return fmt::format("flags:{}", static_cast<std::uint64_t>(arg.Get<Flags>()));
        case ArgType::Operand: {
            const auto op = arg.Get<Operand::Op>();
            return fmt::format(
                    "operand:{}:{}", static_cast<unsigned>(op.type), op.shift_ext);
        }
        case ArgType::Local: {
            const auto local = arg.Get<Local>();
            return fmt::format(
                    "local:{}:{}", local.id, static_cast<unsigned>(local.type));
        }
        case ArgType::Uniform: {
            const auto uniform = arg.Get<Uniform>();
            return fmt::format("uniform:{}:{}",
                               uniform.GetOffset(),
                               static_cast<unsigned>(uniform.GetType()));
        }
        case ArgType::Lambda: {
            const auto lambda = arg.Get<Lambda>();
            if (lambda.IsValue()) {
                const auto value = lambda.GetValue();
                return fmt::format(
                        "lambda-value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
            }
            const auto imm = lambda.GetImm();
            return fmt::format(
                    "lambda-imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Params: {
            std::string out{"params"};
            for (const auto& param : arg.Get<Params>()) {
                if (param.data.IsValue()) {
                    out += fmt::format(
                            ":v{}:{}", param.data.value.Id(),
                            static_cast<unsigned>(param.data.value.Type()));
                } else {
                    out += fmt::format(
                            ":i{}:{}", static_cast<unsigned>(param.data.imm.GetType()),
                            param.data.imm.Get());
                }
            }
            return out;
        }
    }
    return "invalid";
}

}  // namespace

TEST_CASE("IR build fast path is field-identical across all argument shapes") {
    using namespace swift::runtime::ir;

    IRBuildFixture legacy{false};
    IRBuildFixture fast{true};
    auto* lhs = legacy.function;
    auto* rhs = fast.function;

    REQUIRE(lhs->MaxBlockCount() == rhs->MaxBlockCount());
    REQUIRE(lhs->MaxInstrCount() == rhs->MaxInstrCount());
    REQUIRE(lhs->MaxLocalCount() == rhs->MaxLocalCount());
    REQUIRE(lhs->GetHIRBlocks().size() == rhs->GetHIRBlocks().size());
    REQUIRE(lhs->GetHIRBlocksRPO().size() == rhs->GetHIRBlocksRPO().size());

    // The first instruction was allocated before >1536 later live objects.
    // Its address and initialized fields must remain valid after arena growth.
    REQUIRE(legacy.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(fast.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(legacy.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);
    REQUIRE(fast.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);

    for (std::size_t block_index = 0; block_index < lhs->GetHIRBlocks().size();
         ++block_index) {
        auto* left_hir = lhs->GetHIRBlocks()[block_index];
        auto* right_hir = rhs->GetHIRBlocks()[block_index];
        INFO("block " << block_index);
        REQUIRE(left_hir->GetOrderId() == right_hir->GetOrderId());
        REQUIRE(left_hir->GetPredecessors().size() == right_hir->GetPredecessors().size());
        REQUIRE(left_hir->GetSuccessors().size() == right_hir->GetSuccessors().size());
        for (std::size_t i = 0; i < left_hir->GetPredecessors().size(); ++i) {
            REQUIRE(left_hir->GetPredecessors()[i]->GetOrderId() ==
                    right_hir->GetPredecessors()[i]->GetOrderId());
        }
        for (std::size_t i = 0; i < left_hir->GetSuccessors().size(); ++i) {
            REQUIRE(left_hir->GetSuccessors()[i]->GetOrderId() ==
                    right_hir->GetSuccessors()[i]->GetOrderId());
        }

        auto* left_block = left_hir->GetBlock();
        auto* right_block = right_hir->GetBlock();
        REQUIRE(left_block->GetStartLocation() == right_block->GetStartLocation());
        REQUIRE(left_block->GetEndLocation() == right_block->GetEndLocation());
        REQUIRE(fmt::format("{}", left_block->GetTerminal()) ==
                fmt::format("{}", right_block->GetTerminal()));

        auto left_it = left_block->GetInstList().begin();
        auto right_it = right_block->GetInstList().begin();
        for (; left_it != left_block->GetInstList().end() &&
               right_it != right_block->GetInstList().end();
             ++left_it, ++right_it) {
            const auto& left_inst = *left_it;
            const auto& right_inst = *right_it;
            INFO("instruction id " << left_inst.Id());
            REQUIRE(left_inst.GetOp() == right_inst.GetOp());
            REQUIRE(left_inst.Id() == right_inst.Id());
            REQUIRE(left_inst.ReturnType() == right_inst.ReturnType());
            REQUIRE(left_inst.VirRegID() == right_inst.VirRegID());
            REQUIRE(const_cast<Inst&>(left_inst).GetUses(false) ==
                    const_cast<Inst&>(right_inst).GetUses(false));
            for (unsigned slot = 0; slot < Inst::max_args; ++slot) {
                INFO("physical argument slot " << slot);
                REQUIRE(IRSnapshotArg(left_inst.ArgAt(slot)) ==
                        IRSnapshotArg(right_inst.ArgAt(slot)));
            }
            auto left_pseudo = const_cast<Inst&>(left_inst).GetPseudoOperations();
            auto right_pseudo = const_cast<Inst&>(right_inst).GetPseudoOperations();
            REQUIRE(left_pseudo.size() == right_pseudo.size());
            for (std::size_t i = 0; i < left_pseudo.size(); ++i) {
                REQUIRE(left_pseudo[i]->Id() == right_pseudo[i]->Id());
                REQUIRE(left_pseudo[i]->GetOp() == right_pseudo[i]->GetOp());
            }
        }
        REQUIRE(left_it == left_block->GetInstList().end());
        REQUIRE(right_it == right_block->GetInstList().end());
    }

    const auto& left_values = lhs->GetHIRValues();
    const auto& right_values = rhs->GetHIRValues();
    REQUIRE(left_values.size() == right_values.size());
    for (std::size_t id = 0; id < left_values.size(); ++id) {
        auto* left_value = left_values[id];
        auto* right_value = right_values[id];
        INFO("HIR value id " << id);
        REQUIRE((left_value == nullptr) == (right_value == nullptr));
        if (!left_value) {
            continue;
        }
        REQUIRE(left_value->value.Id() == right_value->value.Id());
        REQUIRE(left_value->value.Type() == right_value->value.Type());
        REQUIRE(left_value->block->GetOrderId() == right_value->block->GetOrderId());
        REQUIRE(left_value->allocated.type == right_value->allocated.type);

        auto left_use = left_value->uses.begin();
        auto right_use = right_value->uses.begin();
        for (; left_use != left_value->uses.end() && right_use != right_value->uses.end();
             ++left_use, ++right_use) {
            REQUIRE(left_use->inst->Id() == right_use->inst->Id());
            REQUIRE(left_use->arg_idx == right_use->arg_idx);
        }
        REQUIRE(left_use == left_value->uses.end());
        REQUIRE(right_use == right_value->uses.end());
    }
}

TEST_CASE("Uniform elimination preserves rotate-by-zero carry polarity load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // cmp si,sp; rol r15b,cl; lahf; seto r15b; hlt
    std::array<swift::u8, 13> code{
            0x66, 0x44, 0x39, 0xe6, 0x41, 0xd2, 0xc7,
            0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, FeatureSet{}};
    decoder.Decode();

    UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
    UniformEliminationPass::Run(&block, info, FeatureSet{});

    const auto polarity_offset = offsetof(ThreadContext64, carry_inverted);
    size_t polarity_loads = 0;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::LoadUniform &&
            inst.GetArg<Uniform>(0).GetOffset() == polarity_offset) {
            polarity_loads++;
        }
    }
    REQUIRE(polarity_loads == 1);
}

TEST_CASE("structured V128 address wraps inside the 4GB guest window") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* window = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(window != MAP_FAILED);

    const std::array<swift::u8, 16> expected{
            0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
            0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    };
    std::memcpy(window + 0x10, expected.data(), expected.size());

    // movaps xmm0,[rax+0x18]; hlt
    // 0xfffffff8 + 0x18 = 0x100000010, whose 4GB-window address is 0x10.
    constexpr swift::u64 code_guest = 0x1000;
    const std::array<swift::u8, 5> code{0x0f, 0x28, 0x40, 0x18, 0xf4};
    REQUIRE(code_guest + code.size() < page * 2);
    std::memcpy(window + code_guest, code.data(), code.size());

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make(window, UINT32_MAX);
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = code_guest;
    context.rax.qword = UINT64_C(0xfffffff8);

    const auto exit = core->Run();
    INFO("SVM_ADDRMODE_STRUCT="
         << (swift::runtime::GetRawSvmConfigEnvForTest("SVM_ADDRMODE_STRUCT")
                     ? swift::runtime::GetRawSvmConfigEnvForTest("SVM_ADDRMODE_STRUCT")
                     : "<unset>"));
    REQUIRE(exit == ExitReason::None);
    REQUIRE(std::memcmp(&context.xmm0, expected.data(), expected.size()) == 0);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(window, page * 2);
}

TEST_CASE("A1 host-base fold gate is exact and Q UXTW encoding is valid") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend::arm64;
    using namespace vixl::aarch64;

    REQUIRE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                 ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(false, true, UINT32_MAX,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, false, UINT32_MAX,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, 0,
                                       ir::ValueType::V128, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::U64, true, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, false, true, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, true, false, false));
    REQUIRE_FALSE(HostBaseFoldEligible(true, true, UINT32_MAX,
                                       ir::ValueType::V128, true, true, true));

    MacroAssembler masm;
    masm.Ldr(q0, MemOperand{x24, w10, UXTW});
    masm.Str(q1, MemOperand{x24, w11, UXTW});
    masm.FinalizeCode();
    auto* first = masm.GetBuffer()->GetStartAddress<const Instruction*>();
    auto* second = first->GetNextInstruction();
    REQUIRE(first[0].GetInstructionBits() == 0x3cea4b00u);
    REQUIRE(second->GetInstructionBits() == 0x3cab4b01u);

    Decoder decoder;
    Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    decoder.Decode(&first[0]);
    REQUIRE(std::string_view(disassembler.GetOutput()).find(
                    "ldr q0, [x24, w10, uxtw]") != std::string_view::npos);
    decoder.Decode(second);
    REQUIRE(std::string_view(disassembler.GetOutput()).find(
                    "str q1, [x24, w11, uxtw]") != std::string_view::npos);
}

TEST_CASE("two structured V128 accesses fault on the second page after the first commits") {
    using namespace swift::translator;
    using namespace swift::translator::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* data = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    auto* code_page = static_cast<swift::u8*>(
            mmap(nullptr, page, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(data != MAP_FAILED);
    REQUIRE(code_page != MAP_FAILED);

    const std::array<swift::u8, 16> first{
            0x01, 0x12, 0x23, 0x34, 0x45, 0x56, 0x67, 0x78,
            0x89, 0x9a, 0xab, 0xbc, 0xcd, 0xde, 0xef, 0xf0,
    };
    std::memcpy(data + page - first.size(), first.data(), first.size());
    REQUIRE(mprotect(data + page, page, PROT_NONE) == 0);

    // movaps xmm0,[rax]; movaps xmm1,[rax+0x10]; hlt
    const std::array<swift::u8, 8> code{
            0x0f, 0x28, 0x00, 0x0f, 0x28, 0x48, 0x10, 0xf4,
    };
    std::memcpy(code_page, code.data(), code.size());

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make();
    auto* core = X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<swift::u64>(code_page);
    context.rax.qword = reinterpret_cast<swift::u64>(data + page - first.size());
    constexpr swift::u64 rsi_before = UINT64_C(0x9b8a796857463524);
    context.rsi.qword = rsi_before;
    std::memset(&context.xmm0, 0x5a, sizeof(context.xmm0));
    std::memset(&context.xmm1, 0xa5, sizeof(context.xmm1));
    std::array<swift::u8, 16> second_before{};
    std::memcpy(second_before.data(), &context.xmm1, second_before.size());

    REQUIRE(core->Run() == ExitReason::PageFatal);
    // The two guest memory nodes stay separate and ordered: the first value is
    // architecturally visible, while the faulting second load commits nothing.
    REQUIRE(std::memcmp(&context.xmm0, first.data(), first.size()) == 0);
    REQUIRE(std::memcmp(&context.xmm1,
                        second_before.data(),
                        second_before.size()) == 0);
    REQUIRE(context.rsi.qword == rsi_before);

    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    REQUIRE(mprotect(data + page, page, PROT_READ | PROT_WRITE) == 0);
    munmap(data, page * 2);
    munmap(code_page, page);
}

TEST_CASE("structured address reloads RAX after every partial alias write") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::translator;
    using namespace swift::translator::x86;
    using namespace swift::x86;

    const long page_long = sysconf(_SC_PAGESIZE);
    REQUIRE(page_long > 0);
    const auto page = static_cast<size_t>(page_long);
    auto* window = static_cast<swift::u8*>(
            mmap(nullptr, page * 2, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON, -1, 0));
    REQUIRE(window != MAP_FAILED);

    const std::array<swift::u8, 16> first{
            0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
            0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const std::array<swift::u8, 16> second{
            0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
            0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
    };
    std::memcpy(window + 0x100, first.data(), first.size());
    std::memcpy(window + 0x180, second.data(), second.size());

    struct AliasCase {
        const char* name;
        std::vector<swift::u8> write;
    };
    const std::array cases{
            AliasCase{"AL", {0xb0, 0x80}},
            AliasCase{"AX", {0x66, 0xb8, 0x80, 0x01}},
            AliasCase{"EAX", {0xb8, 0x80, 0x01, 0x00, 0x00}},
    };

    struct WindowMemory final : MemoryInterface {
        explicit WindowMemory(swift::u8* base) : base(base) {}
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, base + addr, size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(base + addr, src, size);
        }
        void* GetPointer(void* src) override {
            return base + reinterpret_cast<uintptr_t>(src);
        }
        swift::u8* base;
    } decode_memory{window};

    swift::runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = X86Instance::Make(window, UINT32_MAX);
    auto* core = X86Core::Make(instance);
    for (size_t i = 0; i < cases.size(); ++i) {
        INFO("partial alias " << cases[i].name);
        const swift::u64 code_guest = 0x1000 + i * 0x100;
        std::vector<swift::u8> code{0x0f, 0x28, 0x00};  // movaps xmm0,[rax]
        code.insert(code.end(), cases[i].write.begin(), cases[i].write.end());
        code.insert(code.end(), {0x0f, 0x28, 0x08, 0xf4});  // movaps xmm1,[rax]; hlt
        REQUIRE(code_guest + code.size() < page * 2);
        std::memcpy(window + code_guest, code.data(), code.size());

        // Structural half of the test: in ON mode both memory nodes carry the
        // address state LoadUniform directly, and the alias write forces two
        // distinct definitions rather than reusing the first snapshot.
        Block block{0, Location{code_guest}};
        Assembler assembler{&block};
        X64Decoder decoder{code_guest, &decode_memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();
        std::vector<Inst*> v128_loads;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() == OpCode::LoadMemory &&
                inst.ReturnType() == ValueType::V128) {
                v128_loads.push_back(&inst);
            }
        }
        REQUIRE(v128_loads.size() == 2);
        const bool structured = swift::runtime::GetSvmConfig().addrmode_struct;
        if (structured) {
            const auto first_addr = v128_loads[0]->GetArg<Operand>(0);
            const auto second_addr = v128_loads[1]->GetArg<Operand>(0);
            REQUIRE(first_addr.GetRight().Null());
            REQUIRE(second_addr.GetRight().Null());
            REQUIRE(first_addr.GetLeft().IsValue());
            REQUIRE(second_addr.GetLeft().IsValue());
            REQUIRE(first_addr.GetLeft().value.Def()->GetOp() == OpCode::LoadUniform);
            REQUIRE(second_addr.GetLeft().value.Def()->GetOp() == OpCode::LoadUniform);
            REQUIRE(first_addr.GetLeft().value != second_addr.GetLeft().value);
        }

        auto& context = core->GetContext();
        context.rip.qword = code_guest;
        context.rax.qword = 0x100;
        std::memset(&context.xmm0, 0, sizeof(context.xmm0));
        std::memset(&context.xmm1, 0, sizeof(context.xmm1));
        REQUIRE(core->Run() == ExitReason::None);
        REQUIRE(std::memcmp(&context.xmm0, first.data(), first.size()) == 0);
        REQUIRE(std::memcmp(&context.xmm1, second.data(), second.size()) == 0);
    }
    X86Core::Destroy(core);
    X86Instance::Destroy(instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(window, page * 2);
}

TEST_CASE("FEX-style vector immediate shift boundary IR") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // PSLLQ xmm0,{0,63,64,255}; PSRLQ xmm0,{0,63,64,255};
    // PSLLDQ xmm0,{0,15,16}; PSRLDQ xmm0,{0,15,16}; HLT.
    // These are the x86 semantic cliffs which must never be reduced modulo
    // the element/vector width by an AArch64 immediate encoding.
    std::vector<swift::u8> code;
    const auto append = [&](std::initializer_list<swift::u8> bytes) {
        code.insert(code.end(), bytes.begin(), bytes.end());
    };
    for (swift::u8 count : {swift::u8{0}, swift::u8{63}, swift::u8{64}, swift::u8{255}}) {
        append({0x66, 0x0F, 0x73, 0xF0, count});  // /6 PSLLQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{63}, swift::u8{64}, swift::u8{255}}) {
        append({0x66, 0x0F, 0x73, 0xD0, count});  // /2 PSRLQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{15}, swift::u8{16}}) {
        append({0x66, 0x0F, 0x73, 0xF8, count});  // /7 PSLLDQ
    }
    for (swift::u8 count : {swift::u8{0}, swift::u8{15}, swift::u8{16}}) {
        append({0x66, 0x0F, 0x73, 0xD8, count});  // /3 PSRLDQ
    }
    code.push_back(0xF4);

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, FeatureSet{}};
    decoder.Decode();

    std::vector<swift::u64> left_counts;
    std::vector<swift::u64> right_counts;
    std::vector<std::pair<swift::u64, bool>> byte_shifts;
    size_t variable_shifts = 0;
    size_t shared_zeroes = 0;
    for (auto& inst : block.GetInstList()) {
        switch (inst.GetOp()) {
            case OpCode::VecShiftLeftImm:
                REQUIRE(inst.GetArg<Imm>(2).Get() == 64);
                left_counts.push_back(inst.GetArg<Imm>(1).Get());
                break;
            case OpCode::VecShiftRightImm:
                REQUIRE(inst.GetArg<Imm>(2).Get() == 64);
                right_counts.push_back(inst.GetArg<Imm>(1).Get());
                break;
            case OpCode::VecByteShift:
                byte_shifts.emplace_back(
                        inst.GetArg<Imm>(2).Get(),
                        inst.GetArg<Imm>(3).Get() != 0);
                break;
            case OpCode::VecShiftLeft:
            case OpCode::VecShiftRight:
                ++variable_shifts;
                break;
            case OpCode::VecSharedZero:
                ++shared_zeroes;
                break;
            default:
                break;
        }
    }

    REQUIRE(left_counts == std::vector<swift::u64>{0, 63, 64, 255});
    REQUIRE(right_counts == std::vector<swift::u64>{0, 63, 64, 255});
    REQUIRE(variable_shifts == 0);
    // count=0 is identity and emits no byte-shift IR; count=16 is represented
    // by the explicit all-zero vector. Only the two count=15 cases reach EXT.
    REQUIRE((byte_shifts ==
             std::vector<std::pair<swift::u64, bool>>{{15, true}, {15, false}}));
    REQUIRE(shared_zeroes == 4);  // one for each 15 and 16 lowering before CSE
}

TEST_CASE("Constant CSE does not reuse a constant materialized under a branch") {
    using namespace swift::runtime::ir;

    // ConstFoldingPass deduplicates LoadImm within a sliding window, and resets
    // its table at Goto / NotGoto / BindLabel because the x86 front end builds
    // intra-block control flow. BindLabel is the reset that carries the
    // correctness: a constant materialized between a forward branch and its
    // label has NOT executed on the path that took the branch, so rewriting a
    // use after the label to that definition reads a register the block never
    // wrote. (The resets at the branch itself are redundant with it -- code
    // before a forward branch dominates everything after -- and are kept only
    // to scope facts to one straight-line region; see the comment in the pass.)
    //
    // The shape is real: DecodeShift (decoder_alu.cc) materializes LoadImm(0)
    // for SAR's OF inside exactly such a NotGoto/BindLabel region, a handful of
    // IR instructions before the label.
    Block conditional{0, Location{0x1000}};
    auto cond = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip = conditional.NotGoto(cond);
    auto guarded = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{0, ValueType::U32}, guarded);
    conditional.BindLabel(skip);
    auto merged = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{8, ValueType::U32}, merged);
    conditional.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&conditional, FeatureSet{});

    auto arg_of_store = [](Block& block, std::uint32_t offset) -> Inst* {
        Inst* found = nullptr;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() == OpCode::StoreUniform &&
                inst.GetArg<Uniform>(0).GetOffset() == offset) {
                found = inst.GetArg<Value>(1).Def();
            }
        }
        return found;
    };

    // The store after the label must still read the constant materialized
    // after the label. Removing the BindLabel reset rewrites it to `guarded`.
    REQUIRE(arg_of_store(conditional, 8) == merged.Def());
    REQUIRE(arg_of_store(conditional, 8) != guarded.Def());

    // Positive control. Without it the case above would also pass if the pass
    // did no deduplication at all -- which is exactly how a barrier ends up
    // "covered" by a test that proves nothing. Same two constants, same
    // distance, no branch between them: here the second one MUST be folded away.
    Block straight_line{1, Location{0x2000}};
    auto first = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{0, ValueType::U32}, first);
    auto second = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{8, ValueType::U32}, second);
    straight_line.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&straight_line, FeatureSet{});

    REQUIRE(arg_of_store(straight_line, 8) == first.Def());
    REQUIRE(arg_of_store(straight_line, 8) != second.Def());
}

TEST_CASE("MMX-form shared opcodes are refused instead of run on the XMM file") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // MMX and SSE share opcode numbers: distorm reports the same insn.opcode
    // for `paddb mm0,mm1` (0F FC C1) and `paddb xmm0,xmm1` (66 0F FC C1), and
    // only the operand register class distinguishes them. x86_regs_table maps
    // R_MM0..R_MM7 onto X86RegInfo::Xmm0..Xmm7, so before the guard in
    // DecodeSwitch an MMX-form instruction did not fail -- it read and wrote
    // the guest's XMM0-XMM7 and produced silently wrong data. This runtime
    // implements no MMX register file and does not advertise MMX (CPUID leaf 1
    // EDX bit 23 is clear), so the only correct answer is to refuse the
    // instruction: InterruptReason::FALLBACK, which the runtime turns into
    // IllegalCode.
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    constexpr auto kInterruptOffset = offsetof(ThreadContext64, interrupt);
    constexpr auto kXmmBase = offsetof(ThreadContext64, xmms);
    constexpr auto kXmmEnd = kXmmBase + sizeof(ThreadContext64::xmms);

    struct Outcome {
        bool refused;      // a FALLBACK interrupt was raised
        bool touched_xmm;  // the XMM register file was read or written
    };
    auto decode = [&](std::vector<swift::u8> bytes) {
        bytes.push_back(0xF4);  // hlt, so Decode() terminates
        const auto address = reinterpret_cast<VAddr>(bytes.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();

        Outcome out{false, false};
        for (auto& inst : block.GetInstList()) {
            const bool is_store = inst.GetOp() == OpCode::StoreUniform;
            const bool is_load = inst.GetOp() == OpCode::LoadUniform;
            if (!is_store && !is_load) {
                continue;
            }
            const auto offset = inst.GetArg<Uniform>(0).GetOffset();
            if (offset >= kXmmBase && offset < kXmmEnd) {
                out.touched_xmm = true;
            }
            if (is_store && offset == kInterruptOffset) {
                auto reason = inst.GetArg<Value>(1);
                if (reason.Defined() && reason.Def()->GetOp() == OpCode::LoadImm &&
                    reason.Def()->GetArg<Imm>(0).Get() ==
                            static_cast<swift::u32>(InterruptReason::FALLBACK)) {
                    out.refused = true;
                }
            }
        }
        return out;
    };

    struct Case {
        const char* name;
        std::vector<swift::u8> mmx;  // no 66 prefix: MM operands
        std::vector<swift::u8> sse;  // 66-prefixed: XMM operands
    };
    // The seven named in the original report, plus a spread across the rest of
    // the shared space (the plain MMX/SSE2 integer set, the SSSE3 additions and
    // the GPR<->vector movers) -- the defect was never specific to seven
    // opcodes, it was a property of the dispatch.
    const std::vector<Case> cases = {
            {"pshufb", {0x0F, 0x38, 0x00, 0xC1}, {0x66, 0x0F, 0x38, 0x00, 0xC1}},
            {"palignr", {0x0F, 0x3A, 0x0F, 0xC1, 0x03}, {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x03}},
            {"pextrw", {0x0F, 0xC5, 0xC1, 0x02}, {0x66, 0x0F, 0xC5, 0xC1, 0x02}},
            {"pmovmskb", {0x0F, 0xD7, 0xC1}, {0x66, 0x0F, 0xD7, 0xC1}},
            {"pmuludq", {0x0F, 0xF4, 0xC1}, {0x66, 0x0F, 0xF4, 0xC1}},
            {"psadbw", {0x0F, 0xF6, 0xC1}, {0x66, 0x0F, 0xF6, 0xC1}},
            {"pminub", {0x0F, 0xDA, 0xC1}, {0x66, 0x0F, 0xDA, 0xC1}},
            {"paddb", {0x0F, 0xFC, 0xC1}, {0x66, 0x0F, 0xFC, 0xC1}},
            {"psubb", {0x0F, 0xF8, 0xC1}, {0x66, 0x0F, 0xF8, 0xC1}},
            {"pand", {0x0F, 0xDB, 0xC1}, {0x66, 0x0F, 0xDB, 0xC1}},
            {"pxor", {0x0F, 0xEF, 0xC1}, {0x66, 0x0F, 0xEF, 0xC1}},
            {"pcmpeqb", {0x0F, 0x74, 0xC1}, {0x66, 0x0F, 0x74, 0xC1}},
            {"punpcklbw", {0x0F, 0x60, 0xC1}, {0x66, 0x0F, 0x60, 0xC1}},
            {"packsswb", {0x0F, 0x63, 0xC1}, {0x66, 0x0F, 0x63, 0xC1}},
            {"pmullw", {0x0F, 0xD5, 0xC1}, {0x66, 0x0F, 0xD5, 0xC1}},
            {"pmaddwd", {0x0F, 0xF5, 0xC1}, {0x66, 0x0F, 0xF5, 0xC1}},
            {"pavgb", {0x0F, 0xE0, 0xC1}, {0x66, 0x0F, 0xE0, 0xC1}},
            {"pmaxsw", {0x0F, 0xEE, 0xC1}, {0x66, 0x0F, 0xEE, 0xC1}},
            {"paddusb", {0x0F, 0xDC, 0xC1}, {0x66, 0x0F, 0xDC, 0xC1}},
            {"psllw imm", {0x0F, 0x71, 0xF0, 0x03}, {0x66, 0x0F, 0x71, 0xF0, 0x03}},
            {"psllw reg", {0x0F, 0xF1, 0xC1}, {0x66, 0x0F, 0xF1, 0xC1}},
            {"pinsrw", {0x0F, 0xC4, 0xC0, 0x02}, {0x66, 0x0F, 0xC4, 0xC0, 0x02}},
            {"movd", {0x0F, 0x6E, 0xC0}, {0x66, 0x0F, 0x6E, 0xC0}},
            {"pabsb", {0x0F, 0x38, 0x1C, 0xC1}, {0x66, 0x0F, 0x38, 0x1C, 0xC1}},
            {"pmulhrsw", {0x0F, 0x38, 0x0B, 0xC1}, {0x66, 0x0F, 0x38, 0x0B, 0xC1}},
            // paddq is the one shared opcode distorm reports with XMM operand
            // indices in BOTH forms, so the register-class test cannot see it
            // and DecodeSwitch reads the 0x66 prefix instead. It is here as a
            // register form and as a memory form, because the prefix scan walks
            // the encoding and the memory form has a different byte layout.
            {"paddq", {0x0F, 0xD4, 0xC1}, {0x66, 0x0F, 0xD4, 0xC1}},
            {"paddq mem", {0x0F, 0xD4, 0x01}, {0x66, 0x0F, 0xD4, 0x01}},
            {"paddq rex", {0x0F, 0xD4, 0xC1}, {0x66, 0x44, 0x0F, 0xD4, 0xC1}},
            // paddq mm0,[rcx+0x66]: the displacement byte IS 0x66. A prefix
            // scan that does not stop at the 0x0F escape reads it as an
            // operand-size prefix and lets this MMX instruction through.
            {"paddq disp=0x66",
             {0x0F, 0xD4, 0x41, 0x66},
             {0x66, 0x0F, 0xD4, 0x41, 0x66}},
    };

    for (auto& c : cases) {
        INFO("MMX form of " << c.name);
        auto mmx = decode(c.mmx);
        // Refusing is the point; not touching the XMM file is what refusing
        // BUYS, and is the assertion that fails if the guard is removed.
        REQUIRE_FALSE(mmx.touched_xmm);
        REQUIRE(mmx.refused);
    }
    for (auto& c : cases) {
        // The 66-prefixed twin must be unaffected: a guard that rejected both
        // forms would pass every assertion above while breaking SSE2.
        INFO("SSE form of " << c.name);
        auto sse = decode(c.sse);
        REQUIRE_FALSE(sse.refused);
        REQUIRE(sse.touched_xmm);
    }
}

TEST_CASE("GPR immediate shifts specialize only nonzero in-range counts") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    struct ShiftFastScope {
        bool had_old{};
        std::string old;
        ShiftFastScope() {
            if (const char* value = swift::runtime::GetRawSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST")) {
                had_old = true;
                old = value;
            }
            swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", "1", 1);
        }
        ~ShiftFastScope() {
            if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", old.c_str(), 1);
            else swift::runtime::UnsetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST");
        }
    } shift_fast_scope;

    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    struct Kind {
        swift::u8 group;
        OpCode imm;
        OpCode value;
    };
    const std::array kinds{
            Kind{4, OpCode::LslImm, OpCode::LslValue},
            Kind{5, OpCode::LsrImm, OpCode::LsrValue},
            Kind{7, OpCode::AsrImm, OpCode::AsrValue},
    };
    const std::array widths{8u, 16u, 32u, 64u};

    auto decode = [&](swift::u32 width, swift::u8 group, swift::u8 count, bool by_cl) {
        std::vector<swift::u8> code;
        if (width == 16) code.push_back(0x66);
        if (width == 64) code.push_back(0x48);
        code.push_back(by_cl ? (width == 8 ? 0xD2 : 0xD3)
                             : (width == 8 ? 0xC0 : 0xC1));
        code.push_back(
                static_cast<swift::u8>(0xC0 | (group << 3)));  // r/m = AL/AX/EAX/RAX
        if (!by_cl) code.push_back(count);
        code.push_back(0xF4);  // hlt terminates Decode

        const auto address = reinterpret_cast<VAddr>(code.data());
        auto block = std::make_unique<Block>(0, Location{address});
        Assembler assembler{block.get()};
        X64Decoder decoder{address, &memory, &assembler, true,
                           Arm64Features::None, false, false, FeatureSet{}};
        decoder.Decode();
        return block;
    };

    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            for (swift::u32 raw_count : counts) {
                INFO("group=" << unsigned(kind.group) << " width=" << width
                              << " count=" << raw_count);
                auto block =
                        decode(width, kind.group, static_cast<swift::u8>(raw_count), false);
                bool has_imm = false;
                bool has_value = false;
                bool has_guard = false;
                bool has_flag_write = false;
                for (auto& inst : block->GetInstList()) {
                    has_imm |= inst.GetOp() == kind.imm;
                    has_value |= inst.GetOp() == kind.value;
                    has_guard |= inst.GetOp() == OpCode::NotGoto;
                    has_flag_write |= inst.GetOp() == OpCode::SaveFlags ||
                                      inst.GetOp() == OpCode::SetCarry ||
                                      inst.GetOp() == OpCode::SetOverflow;
                }

                const swift::u32 effective = raw_count & (width == 64 ? 0x3Fu : 0x1Fu);
                if (effective == 0) {
                    REQUIRE_FALSE(has_imm);
                    REQUIRE_FALSE(has_value);
                    REQUIRE_FALSE(has_guard);
                    REQUIRE_FALSE(has_flag_write);
                } else if (effective < width) {
                    REQUIRE(has_imm);
                    REQUIRE_FALSE(has_value);
                    REQUIRE_FALSE(has_guard);
                    REQUIRE(has_flag_write);
                } else {
                    // 8/16-bit counts at or above the operand width retain the
                    // generic 32-bit-container path and its zero-count guard.
                    REQUIRE(has_value);
                    REQUIRE(has_guard);
                    REQUIRE(has_flag_write);
                }
            }

            // CL is runtime data even if the test initializes it later; it must
            // retain x86's 5/6-bit mask and zero-count flag guard.
            auto cl_block = decode(width, kind.group, 0, true);
            bool has_value = false;
            bool has_guard = false;
            for (auto& inst : cl_block->GetInstList()) {
                has_value |= inst.GetOp() == kind.value;
                has_guard |= inst.GetOp() == OpCode::NotGoto;
            }
            REQUIRE(has_value);
            REQUIRE(has_guard);
        }
    }
}

TEST_CASE("GPR immediate shift boundary results and defined flags execute correctly") {
    using namespace swift::x86;

    struct ShiftFastScope {
        bool had_old{};
        std::string old;
        ShiftFastScope() {
            if (const char* value = swift::runtime::GetRawSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST")) {
                had_old = true;
                old = value;
            }
            swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", "1", 1);
        }
        ~ShiftFastScope() {
            if (had_old) swift::runtime::SetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST", old.c_str(), 1);
            else swift::runtime::UnsetSvmConfigEnvForTest("SVM_SHIFT_IMM_FAST");
        }
    } shift_fast_scope;

    constexpr swift::u64 input = UINT64_C(0x8123456789ab8181);
    struct Kind {
        swift::u8 group;
    };
    const std::array kinds{Kind{4}, Kind{5}, Kind{7}};
    const std::array widths{8u, 16u, 32u, 64u};
    constexpr size_t stride = 32;
    constexpr size_t case_count = 3 * 4 * 5;

    void* guest_code = mmap(nullptr, stride * case_count, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    auto* code_base = static_cast<swift::u8*>(guest_code);
    size_t case_index = 0;
    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            for (swift::u32 raw_count : counts) {
                auto* p = code_base + case_index++ * stride;
                // cmp rbx,rbx; stc gives a known pre-shift state:
                // CF=PF=ZF=1, SF=OF=0. AF is intentionally not checked.
                *p++ = 0x48;
                *p++ = 0x39;
                *p++ = 0xDB;
                *p++ = 0xF9;
                if (width == 16) *p++ = 0x66;
                if (width == 64) *p++ = 0x48;
                *p++ = width == 8 ? 0xC0 : 0xC1;
                *p++ = static_cast<swift::u8>(0xC0 | (kind.group << 3));
                *p++ = static_cast<swift::u8>(raw_count);
                // Preserve the shifted value before LAHF overwrites AH, then
                // capture OF separately in DL.
                *p++ = 0x49;
                *p++ = 0x89;
                *p++ = 0xC0;  // mov r8,rax
                *p++ = 0x9F;  // lahf
                *p++ = 0x0F;
                *p++ = 0x90;
                *p++ = 0xC2;  // seto dl
                *p++ = 0xF4;  // hlt
            }
        }
    }
    REQUIRE(case_index == case_count);

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    case_index = 0;
    for (auto kind : kinds) {
        for (swift::u32 width : widths) {
            const std::array counts{0u, 1u, width - 1, width, width + 1};
            const swift::u64 width_mask =
                    width == 64 ? UINT64_MAX : ((UINT64_C(1) << width) - 1);
            const swift::u64 original = input & width_mask;
            const bool original_sign = ((original >> (width - 1)) & 1) != 0;
            for (swift::u32 raw_count : counts) {
                INFO("group=" << unsigned(kind.group) << " width=" << width
                              << " count=" << raw_count);
                const swift::u32 count = raw_count & (width == 64 ? 0x3Fu : 0x1Fu);
                swift::u64 lane = original;
                if (count != 0) {
                    if (kind.group == 4) {
                        lane = count >= width ? 0 : (original << count) & width_mask;
                    } else if (kind.group == 5) {
                        lane = count >= width ? 0 : original >> count;
                    } else if (count >= width) {
                        lane = original_sign ? width_mask : 0;
                    } else {
                        lane = original >> count;
                        if (original_sign) {
                            const auto retained = (UINT64_C(1) << (width - count)) - 1;
                            lane |= width_mask & ~retained;
                        }
                    }
                }
                const swift::u64 expected_result =
                        width == 64 ? lane
                                    : (width == 32 ? lane
                                                   : ((input & ~width_mask) | lane));

                context.rip.qword =
                        reinterpret_cast<swift::u64>(code_base + case_index++ * stride);
                context.rax.qword = input;
                context.rbx.qword = UINT64_C(0x1122334455667788);
                context.rdx.qword = 0;
                REQUIRE(core->Run() == swift::translator::ExitReason::None);
                REQUIRE(context.r8.qword == expected_result);

                const swift::u8 captured = context.rax.low.low.high;
                swift::u8 flag_mask = 0xC4;  // SF/ZF/PF
                swift::u8 expected = 0;
                if (count == 0) {
                    flag_mask |= 0x01;
                    expected = 0x45;  // preserved CF/PF/ZF
                } else {
                    expected |= lane == 0 ? 0x40 : 0;
                    expected |= ((lane >> (width - 1)) & 1) ? 0x80 : 0;
                    expected |= (std::popcount(static_cast<swift::u8>(lane)) & 1) == 0
                                        ? 0x04
                                        : 0;
                    if (count < width) {
                        flag_mask |= 0x01;
                        const swift::u64 cf =
                                kind.group == 4 ? (original >> (width - count)) & 1
                                                : (original >> (count - 1)) & 1;
                        expected |= static_cast<swift::u8>(cf);
                    }
                }
                REQUIRE((captured & flag_mask) == (expected & flag_mask));

                if (count == 0 || count == 1) {
                    const swift::u8 expected_of =
                            count == 0
                                    ? 0
                                    : (kind.group == 4
                                               ? static_cast<swift::u8>(
                                                         ((lane >> (width - 1)) & 1) ^
                                                         ((original >> (width - 1)) & 1))
                                               : (kind.group == 5 ? original_sign : false));
                    REQUIRE(context.rdx.low.low.low == expected_of);
                }
            }
        }
    }

    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, stride * case_count);
}

TEST_CASE("Uniform elimination uses the latest full GPR store for a narrow load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // sub rbx,rdx; popcnt r10w,bx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x48, 0x29, 0xd3, 0xf3, 0x66, 0x44, 0x0f, 0xb8,
            0xd3, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true,
                       Arm64Features::None, false, false, FeatureSet{}};
    decoder.Decode();

    const auto rbx_offset = offsetof(ThreadContext64, rbx);
    Value latest_rbx_store{};
    Inst* narrow_rbx_load = nullptr;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::StoreUniform &&
            inst.GetArg<Uniform>(0).GetOffset() == rbx_offset) {
            latest_rbx_store = inst.GetArg<Value>(1);
        } else if (inst.GetOp() == OpCode::LoadUniform) {
            auto uniform = inst.GetArg<Uniform>(0);
            if (uniform.GetOffset() == rbx_offset &&
                uniform.GetType() == ValueType::U16) {
                narrow_rbx_load = &inst;
            }
        }
    }
    REQUIRE(latest_rbx_store.Defined());
    REQUIRE(narrow_rbx_load != nullptr);

    UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
    UniformEliminationPass::Run(&block, info, FeatureSet{});

    REQUIRE(narrow_rbx_load->GetOp() == OpCode::BitExtract);
    REQUIRE(narrow_rbx_load->GetArg<Value>(0) == latest_rbx_store);

    // Execute the same sequence without Unicorn. The old bug passed the full
    // post-sub RBX value to Popcnt64 after the narrow load was folded.
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<VAddr>(guest_code);
    context.rbx.qword = UINT64_MAX;
    // RBX - RDX = 0xffffffffffff0003. Its full-width popcount is 50,
    // while popcnt BX must see only 0x0003 and return 2.
    context.rdx.qword = 0xFFFC;
    context.r10.qword = 0xFFFF0000;
    core->Run();
    const auto popcount = context.r10.qword & 0xFFFF;
    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);
    REQUIRE(popcount == 2);
}

TEST_CASE("JIT BitExtract feeds an exact U16 value to CallLambda") {
    using namespace swift::runtime;
    using namespace swift::x86;

    // add cx,r8w; popcnt bx,cx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x66, 0x44, 0x01, 0xc1, 0xf3, 0x66, 0x0f, 0xb8,
            0xd9, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();

    bool matched = true;
    swift::u64 failed_rcx{};
    swift::u64 failed_r8{};
    swift::u16 expected{};
    swift::u16 actual{};
    swift::u64 random = 0x9E3779B97F4A7C15ull;
    for (int iteration = 0; iteration < 1024; ++iteration) {
        random ^= random << 7;
        random ^= random >> 9;
        auto rcx = random;
        random ^= random << 8;
        auto r8 = random;

        auto* core = swift::translator::x86::X86Core::Make(instance);
        auto& context = core->GetContext();
        context.rip.qword = reinterpret_cast<VAddr>(guest_code);
        context.rcx.qword = rcx;
        context.r8.qword = r8;
        context.rbx.qword = 0xA4297CCD266D0000ull;
        core->Run();
        expected = static_cast<swift::u16>(
                std::popcount(static_cast<swift::u16>(rcx + r8)));
        actual = static_cast<swift::u16>(context.rbx.qword);
        swift::translator::x86::X86Core::Destroy(core);
        if (actual != expected) {
            matched = false;
            failed_rcx = rcx;
            failed_r8 = r8;
            break;
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);

    INFO(fmt::format("rcx={:#x}, r8={:#x}, expected={}, actual={}",
                     failed_rcx, failed_r8, expected, actual));
    REQUIRE(matched);
}

TEST_CASE("Test x86 translator") {
    using namespace swift::x86;
    using namespace swift::translator::x86;
    auto instance = X86Instance::Make();
    auto core1 = X86Core::Make(instance);

    core1->Run();
}

TEST_CASE("IR meta consistency") {
    using namespace swift::runtime::ir;

    // (1) The constexpr projection (ir.inc via meta::kPhys) must agree with the
    // runtime table summed through the single slot rule, PhysicalSlots.
    constexpr int kPhysByOp[] = {
            0,  // OpCode::Void
#define INST(name, ret, ...) meta::kPhys_##name,
#include "runtime/ir/ir.inc"
#undef INST
    };
    for (int op = 1; op < (int)OpCode::BASE_COUNT; op++) {
        auto& info = GetIRMetaInfo((OpCode)op);
        int runtime_slots = 0;
        for (auto t : info.arg_types) {
            runtime_slots += PhysicalSlots(t);
        }
        REQUIRE(runtime_slots == kPhysByOp[op]);
        REQUIRE(runtime_slots <= Inst::max_args);
    }

    // (2) StoreMemory(Operand, Value): GetArg<Value>(1) must resolve to the right
    // physical slot without the old PublicIndex out-of-bounds read.
    Block block{0, Location{0x1000}};
    auto base = block.LoadImm(Imm{0x1000u});
    auto idx = block.LoadImm(Imm{0x8u});
    auto stored = block.LoadImm(Imm{42u});
    Operand operand{base, idx};
    auto* store = block.AppendInst(OpCode::StoreMemory, operand, stored);
    REQUIRE(store->GetArg<Value>(1) == stored);

    // (3) Operand round-trip: GetArg<Operand>(0) rebuilds op/left/right.
    Operand got = store->GetArg<Operand>(0);
    REQUIRE(got.GetOp() == operand.GetOp());
    REQUIRE(got.GetLeft().IsValue());
    REQUIRE(got.GetLeft().value == base);
    REQUIRE(got.GetRight().IsValue());
    REQUIRE(got.GetRight().value == idx);
}

TEST_CASE("Flag elimination keeps live pseudo masks and unlinks dead pseudos") {
    using namespace swift::runtime::ir;

    // Dead-pseudo removal, exercised on a carry-free mask. A SaveFlags whose
    // bits are all rewritten before any read is dropped, and the surviving
    // write keeps its original (un-narrowed) mask.
    Block overwritten{0, Location{0x1000}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto old_result = overwritten.Add(lhs, Operand{rhs});
    overwritten.AppendInst(OpCode::SaveFlags, old_result, Flags::NZ);
    auto new_result = overwritten.Add(lhs, Operand{rhs});
    auto* new_save = overwritten.AppendInst(OpCode::SaveFlags, new_result, Flags::NZ);

    FlagsEliminationPass::Run(&overwritten, nullptr, FeatureSet{});

    REQUIRE(old_result.Def()->GetPseudoOperations(OpCode::SaveFlags).empty());
    auto new_pseudos = new_result.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(new_pseudos.size() == 1);
    REQUIRE(new_pseudos[0] == new_save);
    REQUIRE(new_pseudos[0]->GetArg<Flags>(1) == Flags::NZ);
    size_t save_count = 0;
    for (auto& inst : overwritten.GetInstList()) {
        save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(save_count == 1);

    // A carry write covered by a later carry write in the same block is dead.
    // SVM_FLAG_CARRY_ELIM=0 retains the old Gate B behavior for bisection.
    Block carry_overwritten{0, Location{0x1800}};
    auto c_lhs = carry_overwritten.LoadImm(Imm{1u});
    auto c_rhs = carry_overwritten.LoadImm(Imm{2u});
    auto c_old = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_old, Flags::All);
    auto c_new = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_new, Flags::All);

    FlagsEliminationPass::Run(&carry_overwritten, nullptr, FeatureSet{});

    const bool carry_elim_off =
            !swift::runtime::GetSvmConfig().flag_carry_elim;
    REQUIRE(c_old.Def()->GetPseudoOperations(OpCode::SaveFlags).size() ==
            (carry_elim_off ? 1 : 0));
    REQUIRE(c_new.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    size_t carry_save_count = 0;
    for (auto& inst : carry_overwritten.GetInstList()) {
        carry_save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(carry_save_count == (carry_elim_off ? 2 : 1));

    Block carry_read{0, Location{0x2000}};
    auto carry_lhs = carry_read.LoadImm(Imm{5u});
    auto carry_rhs = carry_read.LoadImm(Imm{3u});
    auto carry_source = carry_read.Add(carry_lhs, Operand{carry_rhs});
    auto* carry_save = carry_read.AppendInst(OpCode::SaveFlags, carry_source, Flags::All);
    auto sbb_result = carry_read.Sbb(carry_lhs, Operand{carry_rhs});
    carry_read.AppendInst(OpCode::SaveFlags, sbb_result, Flags::All);

    FlagsEliminationPass::Run(&carry_read, nullptr, FeatureSet{});

    // Sbb reads the preceding carry, so its producer stays live. Its mask must
    // remain whole: the JIT cannot safely turn this into a C-only pseudo.
    auto carry_pseudos = carry_source.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(carry_pseudos.size() == 1);
    REQUIRE(carry_pseudos[0] == carry_save);
    REQUIRE(carry_pseudos[0]->GetArg<Flags>(1) == Flags::All);

    // A reader between two full writes needs only its tested bit from the
    // earlier producer. W14 narrows the surviving pseudo to that bit; the
    // module switch restores the old full mask exactly.
    Block partial{0, Location{0x2800}};
    auto p_old = partial.Add(lhs, Operand{rhs});
    auto* p_old_save = partial.AppendInst(OpCode::SaveFlags, p_old, Flags::All);
    partial.AppendInst(OpCode::TestFlags, Flags::Zero);
    auto p_new = partial.Add(lhs, Operand{rhs});
    partial.AppendInst(OpCode::SaveFlags, p_new, Flags::All);

    FlagsEliminationPass::Run(&partial, nullptr, FeatureSet{});

    const bool full_elim_on = swift::runtime::GetSvmConfig().flag_full_elim;
    REQUIRE(p_old_save->GetArg<Flags>(1) ==
            (full_elim_on ? Flags::Zero : Flags::NZCV));
}

TEST_CASE("Flag elimination removes only overwritten in-block carry writes") {
    using namespace swift::runtime::ir;

    auto count_op = [](Block& block, OpCode op) {
        size_t count = 0;
        for (auto& inst : block.GetInstList()) {
            count += inst.GetOp() == op;
        }
        return count;
    };
    auto contains = [](Block& block, Inst* wanted) {
        for (auto& inst : block.GetInstList()) {
            if (&inst == wanted) {
                return true;
            }
        }
        return false;
    };
    auto append_carry_save = [](Block& block, Value lhs, Value rhs) {
        auto result = block.Add(lhs, Operand{rhs});
        return block.AppendInst(OpCode::SaveFlags, result, Flags::Carry);
    };

    const bool carry_elim_off =
            !swift::runtime::GetSvmConfig().flag_carry_elim;
    if (carry_elim_off) {
        // The bisect switch covers every deletion introduced by this change.
        // Carry writes of all three forms survive exactly as under old Gate B;
        // unrelated non-carry DSE remains enabled.
        Block disabled{0, Location{0x3000}};
        auto lhs = disabled.LoadImm(Imm{1u});
        auto rhs = disabled.LoadImm(Imm{2u});
        auto* old_save = append_carry_save(disabled, lhs, rhs);
        auto* old_clear = disabled.AppendInst(OpCode::ClearFlags, Flags::Carry);
        auto carry_value = disabled.LoadImm<BOOL>(Imm{1u});
        auto* old_set = disabled.AppendInst(OpCode::SetCarry, carry_value);
        auto* old_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);
        auto* last_save = append_carry_save(disabled, lhs, rhs);
        auto* last_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);

        FlagsEliminationPass::Run(&disabled, nullptr, FeatureSet{});

        REQUIRE(contains(disabled, old_save));
        REQUIRE(contains(disabled, old_clear));
        REQUIRE(contains(disabled, old_set));
        REQUIRE_FALSE(contains(disabled, old_noncarry));
        REQUIRE(contains(disabled, last_save));
        REQUIRE(contains(disabled, last_noncarry));
        REQUIRE(count_op(disabled, OpCode::SaveFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::ClearFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::SetCarry) == 1);
        return;
    }

    // (1) Straight line, no intervening reader: the earlier C write is dead.
    Block overwritten{1, Location{0x3100}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto* first = append_carry_save(overwritten, lhs, rhs);
    auto* last = append_carry_save(overwritten, lhs, rhs);

    FlagsEliminationPass::Run(&overwritten, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(overwritten, first));
    REQUIRE(contains(overwritten, last));
    REQUIRE(count_op(overwritten, OpCode::SaveFlags) == 1);

    // (2) A C reader between two writers keeps both writers live.
    Block read_between{2, Location{0x3200}};
    lhs = read_between.LoadImm(Imm{3u});
    rhs = read_between.LoadImm(Imm{4u});
    first = append_carry_save(read_between, lhs, rhs);
    read_between.AppendInst(OpCode::TestFlags, Flags::Carry);
    last = append_carry_save(read_between, lhs, rhs);

    FlagsEliminationPass::Run(&read_between, nullptr, FeatureSet{});

    REQUIRE(contains(read_between, first));
    REQUIRE(contains(read_between, last));
    REQUIRE(count_op(read_between, OpCode::SaveFlags) == 2);

    // (3) Flags::All at block exit makes a lone final C write live.
    Block live_out{3, Location{0x3300}};
    lhs = live_out.LoadImm(Imm{5u});
    rhs = live_out.LoadImm(Imm{6u});
    auto* only = append_carry_save(live_out, lhs, rhs);

    FlagsEliminationPass::Run(&live_out, nullptr, FeatureSet{});

    REQUIRE(contains(live_out, only));
    REQUIRE(count_op(live_out, OpCode::SaveFlags) == 1);

    // (4) On one path C is read before the later covering write; on the other
    // path the branch skips directly to that write. The earlier writer is live
    // on the union of both backward paths.
    Block branched{4, Location{0x3400}};
    lhs = branched.LoadImm(Imm{7u});
    rhs = branched.LoadImm(Imm{8u});
    first = append_carry_save(branched, lhs, rhs);
    auto condition = branched.LoadImm<BOOL>(Imm{1u});
    auto skip_read = branched.NotGoto(condition);
    branched.AppendInst(OpCode::TestFlags, Flags::Carry);
    branched.BindLabel(skip_read);
    last = append_carry_save(branched, lhs, rhs);

    FlagsEliminationPass::Run(&branched, nullptr, FeatureSet{});

    REQUIRE(contains(branched, first));
    REQUIRE(contains(branched, last));
    REQUIRE(count_op(branched, OpCode::SaveFlags) == 2);

    // (5) ClearFlags(C) and SetCarry are ordinary C writers for liveness.
    Block clear_covered{5, Location{0x3500}};
    lhs = clear_covered.LoadImm(Imm{9u});
    rhs = clear_covered.LoadImm(Imm{10u});
    auto* clear = clear_covered.AppendInst(OpCode::ClearFlags, Flags::Carry);
    last = append_carry_save(clear_covered, lhs, rhs);

    FlagsEliminationPass::Run(&clear_covered, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(clear_covered, clear));
    REQUIRE(contains(clear_covered, last));
    REQUIRE(count_op(clear_covered, OpCode::ClearFlags) == 0);

    Block set_covered{6, Location{0x3600}};
    lhs = set_covered.LoadImm(Imm{11u});
    rhs = set_covered.LoadImm(Imm{12u});
    auto carry_value = set_covered.LoadImm<BOOL>(Imm{1u});
    auto* set = set_covered.AppendInst(OpCode::SetCarry, carry_value);
    last = append_carry_save(set_covered, lhs, rhs);

    FlagsEliminationPass::Run(&set_covered, nullptr, FeatureSet{});

    REQUIRE_FALSE(contains(set_covered, set));
    REQUIRE(contains(set_covered, last));
    REQUIRE(count_op(set_covered, OpCode::SetCarry) == 0);

    // (7) Gate A remains block-wide: any Adc/Sbb leaves every instruction
    // untouched, including otherwise-covered C writers.
    Block gate_a{7, Location{0x3700}};
    lhs = gate_a.LoadImm(Imm{13u});
    rhs = gate_a.LoadImm(Imm{14u});
    first = append_carry_save(gate_a, lhs, rhs);
    clear = gate_a.AppendInst(OpCode::ClearFlags, Flags::Carry);
    carry_value = gate_a.LoadImm<BOOL>(Imm{1u});
    set = gate_a.AppendInst(OpCode::SetCarry, carry_value);
    gate_a.Sbb(lhs, Operand{rhs});
    last = append_carry_save(gate_a, lhs, rhs);

    FlagsEliminationPass::Run(&gate_a, nullptr, FeatureSet{});

    REQUIRE(contains(gate_a, first));
    REQUIRE(contains(gate_a, clear));
    REQUIRE(contains(gate_a, set));
    REQUIRE(contains(gate_a, last));
    REQUIRE(count_op(gate_a, OpCode::SaveFlags) == 2);
    REQUIRE(count_op(gate_a, OpCode::ClearFlags) == 1);
    REQUIRE(count_op(gate_a, OpCode::SetCarry) == 1);
}

TEST_CASE("Register allocation gives every spilled value a private slot") {
    using namespace swift::runtime::ir;
    using swift::runtime::backend::RegAlloc;
    using swift::runtime::backend::GPRSMask;
    using swift::runtime::backend::FPRSMask;

    // Two live values sharing one State::spill_area slot is a silent
    // miscompile: both Str/Ldr target the same address, so each reload
    // observes the other's value. Squeeze the linear scan onto its spill path
    // and assert the slots are disjoint, counting a SIMD spill as the two u64
    // slots its 16-byte access actually covers.
    auto check_disjoint_slots = [](int n_scalar,
                                   int n_vector,
                                   std::uint32_t free_gprs,
                                   std::uint32_t free_fprs) {
        Block block{0, Location{0x1000}};
        std::vector<Value> scalars;
        for (int i = 0; i < n_scalar; i++) {
            scalars.push_back(block.LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        }
        std::vector<Value> vectors;
        for (int i = 0; i < n_vector; i++) {
            vectors.push_back(block.LoadImm<TypedValue<ValueType::V128>>(
                    Imm{static_cast<std::uint64_t>(i + 0x100)}));
        }
        // Every value needs a use to own a live interval, and consuming them
        // in reverse definition order keeps them all live at once.
        for (int i = n_vector - 1; i >= 0; i--) {
            block.VecAdd<TypedValue<ValueType::V128>>(vectors[i], vectors[i], Imm{32u});
        }
        if (!scalars.empty()) {
            Value acc = scalars.back();
            for (int i = n_scalar - 2; i >= 0; i--) {
                acc = block.Add(acc, Operand{scalars[i]});
            }
        }
        block.ReIdInstr();  // AppendInst does not assign ids; the pass indexes by id

        // bit SET = unavailable; leave only the lowest `free_*` clear.
        GPRSMask gprs{~((1u << free_gprs) - 1u)};
        FPRSMask fprs{~((1u << free_fprs) - 1u)};
        RegAlloc reg_alloc{0x200, gprs, fprs, FeatureSet{}};
        RegisterAllocPass::Run(&block, &reg_alloc, false, FeatureSet{});

        std::map<std::uint32_t, int> occupancy;
        int spilled = 0;
        auto record = [&](const std::vector<Value>& values, int slot_width) {
            for (auto& value : values) {
                if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    continue;
                }
                spilled++;
                const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
                for (int k = 0; k < slot_width; k++) {
                    occupancy[slot + k]++;
                }
            }
        };
        record(scalars, 1);
        record(vectors, 2);

        REQUIRE(spilled >= 2);  // the case must actually reach the spill path
        for (auto& [slot, owners] : occupancy) {
            INFO("spill slot " << slot << " claimed by " << owners << " live values");
            REQUIRE(owners == 1);
        }
    };

    check_disjoint_slots(12, 0, 2, 8);  // scalar-only pressure
    check_disjoint_slots(0, 10, 8, 2);  // vector-only pressure
    check_disjoint_slots(10, 6, 2, 2);  // mixed
    check_disjoint_slots(5, 5, 1, 1);   // odd-sized stack before a SIMD pair
}

TEST_CASE("integer width ties require exact last-use and a proven W write") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    const GPRSMask gprs{~((1u << 19) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto check = [&](Block* raw,
                     Value source,
                     Value result,
                     bool expect_off_tie,
                     bool expect_on_tie) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc off{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForIntWidthTieTest(raw, &off, false);
        REQUIRE(off.ValueType(source) == RegAlloc::GPR);
        REQUIRE(off.ValueType(result) == RegAlloc::GPR);
        REQUIRE((off.ValueGPR(source).id == off.ValueGPR(result).id) ==
                expect_off_tie);

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForIntWidthTieTest(raw, &on, true);
        REQUIRE(on.ValueType(source) == RegAlloc::GPR);
        REQUIRE(on.ValueType(result) == RegAlloc::GPR);
        REQUIRE((on.ValueGPR(source).id == on.ValueGPR(result).id) ==
                expect_on_tie);
    };

    SECTION("U32 load producer") {
        auto* block = new Block(0, Location{0x8600});
        auto source = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, true);
    }

    SECTION("U32 ALU producer") {
        auto* block = new Block(0, Location{0x8610});
        auto left = block->LoadImm(Imm{0xffffffffu}).SetType(ValueType::U32);
        auto right = block->LoadImm(Imm{0x12345678u}).SetType(ValueType::U32);
        auto source = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, true);
    }

    SECTION("U32 shift producer remains compatible with the existing tie") {
        auto* block = new Block(0, Location{0x8620});
        auto input = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto source = block->LsrImm(input, Imm{3u}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, true, true);
    }

    SECTION("two consecutive identity ties keep the original W producer") {
        auto* block = new Block(0, Location{0x8630});
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{7u}).SetType(ValueType::U32);
        auto producer = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto extract = block->BitExtract(producer, Imm{0u}, Imm{32u})
                               .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(extract);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);

        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        RegAlloc off{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForIntWidthTieTest(block, &off, false);
        REQUIRE(off.ValueGPR(producer).id != off.ValueGPR(extract).id);
        REQUIRE(off.ValueGPR(extract).id != off.ValueGPR(result).id);

        RegAlloc on{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForIntWidthTieTest(block, &on, true);
        REQUIRE(on.ValueGPR(producer).id == on.ValueGPR(extract).id);
        REQUIRE(on.ValueGPR(extract).id == on.ValueGPR(result).id);
    }

    SECTION("GetHostGPR is a W view, not a W write") {
        auto* block = new Block(0, Location{0x8640});
        auto source = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("identity BitExtract cannot launder a GetHostGPR high half") {
        auto* block = new Block(0, Location{0x8650});
        auto host = block->GetHostGPR(HostRegIndex(0), Imm{0u})
                            .SetType(ValueType::U32);
        auto source = block->BitExtract(host, Imm{0u}, Imm{32u})
                              .SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("pure BitCast is not a physical W write") {
        auto* block = new Block(0, Location{0x8660});
        auto producer = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto source = block->BitCast(producer).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        check(block, source, result, false, false);
    }

    SECTION("a later source use defeats the authoritative last-use proof") {
        auto* block = new Block(0, Location{0x8670});
        auto left = block->LoadUniform<TypedValue<ValueType::U32>>(
                Uniform{0, ValueType::U32});
        auto right = block->LoadImm(Imm{1u}).SetType(ValueType::U32);
        auto source = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto result = block->ZeroExtend32To64(source);
        auto late = block->Add(source, Operand{right}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{8, ValueType::U64}, result);
        block->StoreUniform(Uniform{16, ValueType::U32}, late);
        check(block, source, result, false, false);
    }
}

TEST_CASE("64-bit induction ties require exact last-use and matching pinned publish") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    const GPRSMask gprs{~((1u << 19) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto check = [&](bool use_source_after_add,
                     swift::u16 publish_target,
                     bool active_flags,
                     bool expect_tie) {
        auto* raw = new Block(0, Location{0x86a0});
        swift::runtime::IntrusivePtr<Block> block{raw};
        auto source = raw->GetHostGPR(HostRegIndex(22), Imm{0u})
                              .SetType(ValueType::U64);
        auto immediate = raw->LoadImm(Imm{swift::u64{16}}).SetType(ValueType::U64);
        auto alias = raw->BitCast(source).SetType(ValueType::U64);
        auto result = raw->Add(alias, Operand{immediate}).SetType(ValueType::U64);
        if (active_flags) {
            raw->SaveFlags(result, Flags::All);
        }
        if (use_source_after_add) {
            raw->StoreUniform(Uniform{24, ValueType::U64}, source);
        }
        raw->SetHostGPR(result, HostRegIndex(publish_target), Imm{0u});
        raw->StoreUniform(Uniform{32, ValueType::U64}, result);
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc off{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForInductTieTest(raw, &off, false);
        REQUIRE(off.ValueGPR(source).id == 22);
        REQUIRE(off.ValueGPR(result).id != 22);

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForInductTieTest(raw, &on, true);
        REQUIRE(on.ValueGPR(source).id == 22);
        REQUIRE((on.ValueGPR(result).id == 22) == expect_tie);
    };

    SECTION("exact last use and matching target tie") {
        check(false, 22, false, true);
    }
    SECTION("source live after result start falls back") {
        check(true, 22, false, false);
    }
    SECTION("different architectural target falls back") {
        check(false, 23, false, false);
    }
    SECTION("live full-width flags reject destructive source") {
        check(false, 22, true, false);
    }

    SECTION("later pinned overwrite while result snapshot is live falls back") {
        auto* raw = new Block(0, Location{0x86b0});
        swift::runtime::IntrusivePtr<Block> block{raw};
        auto source = raw->GetHostGPR(HostRegIndex(22), Imm{0u})
                              .SetType(ValueType::U64);
        auto immediate = raw->LoadImm(Imm{swift::u64{32}}).SetType(ValueType::U64);
        auto result = raw->Add(source, Operand{immediate}).SetType(ValueType::U64);
        raw->SetHostGPR(result, HostRegIndex(22), Imm{0u});
        auto next = raw->Add(result, Operand{immediate}).SetType(ValueType::U64);
        raw->SetHostGPR(next, HostRegIndex(22), Imm{0u});
        raw->StoreUniform(Uniform{32, ValueType::U64}, result);
        raw->SetTerminal(terminal::ReturnToDispatch{});
        raw->ReIdInstr();

        RegAlloc on{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::RunForInductTieTest(raw, &on, true);
        REQUIRE(on.ValueGPR(source).id == 22);
        REQUIRE(on.ValueGPR(result).id != 22);
    }
}

TEST_CASE("guest GPR coalescing keeps publication and snapshot proofs local") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    auto pinned_gprs = [] {
        GPRSMask mask{0};
        for (swift::u32 code : {0u, 1u, 2u, 3u, 4u, 5u, 19u, 20u, 21u,
                                22u, 23u, 25u, 26u, 27u, 28u, 29u, 30u, 31u}) {
            mask.Mark(code);
        }
        return mask;
    };
    const FPRSMask fprs{~((1u << 8) - 1u)};

    auto allocate = [&](Block* block, bool enabled) {
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        auto features = FeatureSet{};
        features.ra_coalesce = enabled;
        auto alloc = std::make_unique<RegAlloc>(
                block->MaxInstrId(), pinned_gprs(), fprs, features);
        RegisterAllocPass::RunForCoalesceTest(block, alloc.get(), enabled);
        return alloc;
    };

    struct ExpandedProducer {
        OpCode op;
        const char* name;
    };
    constexpr std::array expanded_producers{
            ExpandedProducer{OpCode::Adc, "Adc"},
            ExpandedProducer{OpCode::Sbb, "Sbb"},
            ExpandedProducer{OpCode::Mul, "Mul"},
            ExpandedProducer{OpCode::Div, "Div"},
            ExpandedProducer{OpCode::AndNot, "AndNot"},
            ExpandedProducer{OpCode::Not, "Not"},
            ExpandedProducer{OpCode::LslImm, "LslImm"},
            ExpandedProducer{OpCode::LslValue, "LslValue"},
            ExpandedProducer{OpCode::LsrImm, "LsrImm"},
            ExpandedProducer{OpCode::LsrValue, "LsrValue"},
            ExpandedProducer{OpCode::AsrImm, "AsrImm"},
            ExpandedProducer{OpCode::AsrValue, "AsrValue"},
            ExpandedProducer{OpCode::RorImm, "RorImm"},
            ExpandedProducer{OpCode::RorValue, "RorValue"},
            ExpandedProducer{OpCode::ByteSwap, "ByteSwap"},
            ExpandedProducer{OpCode::BitExtract, "BitExtract"},
            ExpandedProducer{OpCode::BitClear, "BitClear"},
            ExpandedProducer{OpCode::Select, "Select"},
            ExpandedProducer{OpCode::CondSelect, "CondSelect"},
            ExpandedProducer{OpCode::MulHigh, "MulHigh"},
    };

    auto append_expanded_producer = [](Block* block, OpCode op,
                                       ValueType type) -> Value {
        auto load_scalar = [&](swift::u64 value) {
            return block->LoadImm(Imm{value}).SetType(type);
        };
        auto binary = [&](auto emit) {
            auto left = load_scalar(0x123456789abcdef0ull);
            auto right = load_scalar(0x0102030405060708ull);
            return emit(left, right).SetType(type);
        };
        switch (op) {
            case OpCode::Adc:
                return binary([&](Value left, Value right) {
                    return block->Adc(left, Operand{right});
                });
            case OpCode::Sbb:
                return binary([&](Value left, Value right) {
                    return block->Sbb(left, Operand{right});
                });
            case OpCode::Mul:
                return binary([&](Value left, Value right) {
                    return block->Mul(left, Operand{right});
                });
            case OpCode::Div:
                return binary([&](Value left, Value right) {
                    return block->Div(left, Operand{right});
                });
            case OpCode::AndNot:
                return binary([&](Value left, Value right) {
                    return block->AndNot(left, Operand{right});
                });
            case OpCode::Not:
                return binary([&](Value left, Value right) {
                    return block->Not(left, Operand{right});
                });
            case OpCode::LslImm:
                return block->LslImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::LsrImm:
                return block->LsrImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::AsrImm:
                return block->AsrImm(load_scalar(0x923456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::RorImm:
                return block->RorImm(load_scalar(0x123456789abcdef0ull), Imm{5u})
                        .SetType(type);
            case OpCode::LslValue:
                return block->LslValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::LsrValue:
                return block->LsrValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::AsrValue:
                return block->AsrValue(load_scalar(0x923456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::RorValue:
                return block->RorValue(load_scalar(0x123456789abcdef0ull),
                                       load_scalar(5)).SetType(type);
            case OpCode::ByteSwap:
                return block->ByteSwap(load_scalar(0x123456789abcdef0ull),
                                       Imm{type == ValueType::U64 ? 64u : 32u})
                        .SetType(type);
            case OpCode::BitExtract:
                return block->BitExtract(load_scalar(0x123456789abcdef0ull),
                                         Imm{4u},
                                         Imm{type == ValueType::U64 ? 60u : 28u})
                        .SetType(type);
            case OpCode::BitClear:
                return block->BitClear(load_scalar(0x123456789abcdef0ull),
                                       Imm{8u}, Imm{8u})
                        .SetType(type);
            case OpCode::Select: {
                auto cond = block->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
                auto on_true = load_scalar(0x123456789abcdef0ull);
                auto on_false = load_scalar(0x0102030405060708ull);
                return block->Select(cond, on_true, on_false).SetType(type);
            }
            case OpCode::CondSelect: {
                auto on_true = load_scalar(0x123456789abcdef0ull);
                auto on_false = load_scalar(0x0102030405060708ull);
                return block->CondSelect(Cond::EQ, on_true, on_false)
                        .SetType(type);
            }
            case OpCode::MulHigh:
                return binary([&](Value left, Value right) {
                    return block->MulHigh(left, right, Imm{0u});
                });
            default:
                return {};
        }
    };

    struct ExpandedCase {
        IntrusivePtr<Block> block;
        Value produced;
        Value conflict;
        Inst* publish;
    };
    auto make_expanded_case = [&](OpCode op, ValueType type, swift::u64 location) {
        IntrusivePtr<Block> block{new Block(0, Location{location})};
        auto produced = append_expanded_producer(block.get(), op, type);
        auto conflict = block->LoadImm(Imm{swift::u64{0xfeedfacecafebeef}})
                                .SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, produced, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{24, ValueType::U64}, conflict);
        return ExpandedCase{std::move(block), produced, conflict, publish};
    };

    SECTION("last-use full-width publication enters the fixed home") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86c0})};
        auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                             .SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto off = allocate(block.get(), false);
        REQUIRE(off->ValueGPR(value).id != 22);
        REQUIRE_FALSE(off->IsHostWriteCoalesced(publish->Id()));
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id == 22);
        REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("a later fixed-home write while the value is live rejects publication") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86d0})};
        auto value = block->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto next = block->LoadImm(Imm{swift::u64{22}}).SetType(ValueType::U64);
        block->SetHostGPR(next, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{32, ValueType::U64}, value);
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id != 22);
        REQUIRE_FALSE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("a pre-tied value born inside the publication window rejects coalescing") {
        IntrusivePtr<Block> block{new Block(0, Location{0x86d8})};
        auto candidate = block->LoadImm(Imm{swift::u64{11}}).SetType(ValueType::U64);
        // This definition is between candidate and its logical publication.
        // The conflict selector models an earlier tie assigning it to x22;
        // its use after publish makes that fixed-home interval cross the store.
        auto tied = block->LoadImm(Imm{swift::u64{22}}).SetType(ValueType::U64);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, candidate, HostRegIndex(22), Imm{0u});
        block->StoreUniform(Uniform{24, ValueType::U64}, tied);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();

        auto features = FeatureSet{};
        features.ra_coalesce = true;
        RegAlloc control{block->MaxInstrId(), pinned_gprs(), fprs, features};
        RegisterAllocPass::RunForCoalesceTest(block.get(), &control, true);
        REQUIRE(control.IsHostWriteCoalesced(publish->Id()));

        RegAlloc conflict{block->MaxInstrId(), pinned_gprs(), fprs, features};
        RegisterAllocPass::RunForCoalesceConflictTest(
                block.get(), &conflict, tied.Id(), 22);
        REQUIRE(conflict.ValueGPR(tied).id == 22);
        REQUIRE_FALSE(conflict.IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("flags and fault observers reject early publication") {
        IntrusivePtr<Block> flag_block{new Block(0, Location{0x86e0})};
        auto flagged = flag_block->LoadImm(Imm{swift::u64{7}}).SetType(ValueType::U64);
        flag_block->SaveFlags(flagged, Flags::All);
        auto* flags_publish = flag_block->AppendInst(
                OpCode::SetHostGPR, flagged, HostRegIndex(22), Imm{0u});
        auto flags_on = allocate(flag_block.get(), true);
        REQUIRE_FALSE(flags_on->IsHostWriteCoalesced(flags_publish->Id()));

        IntrusivePtr<Block> fault{new Block(0, Location{0x86f0})};
        auto value = fault->LoadImm(Imm{swift::u64{9}}).SetType(ValueType::U64);
        auto address = fault->LoadImm(Imm{swift::u64{0x1000}}).SetType(ValueType::U64);
        (void)fault->LoadMemory(Operand{address}).SetType(ValueType::U64);
        auto* fault_publish = fault->AppendInst(
                OpCode::SetHostGPR, value, HostRegIndex(22), Imm{0u});
        auto fault_on = allocate(fault.get(), true);
        REQUIRE_FALSE(fault_on->IsHostWriteCoalesced(fault_publish->Id()));
    }

    SECTION("W-clean publication removes the bridge and feeds a zero-cost U32 read") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8700})};
        auto left = block->LoadImm(Imm{swift::u32{0x1234}}).SetType(ValueType::U32);
        auto right = block->LoadImm(Imm{swift::u32{0x20}}).SetType(ValueType::U32);
        auto value = block->Add(left, Operand{right}).SetType(ValueType::U32);
        auto zext = block->ZeroExtend32To64(value);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, zext, HostRegIndex(22), Imm{0u});
        auto read = block->GetHostGPR(HostRegIndex(22), Imm{0u})
                            .SetType(ValueType::U32);
        block->StoreUniform(Uniform{40, ValueType::U32}, read);
        // An unrelated full-width write makes the old block-wide pin-chain
        // fuse decline this block, so this section exercises only the new gate.
        auto unrelated = block->LoadImm(Imm{swift::u64{3}}).SetType(ValueType::U64);
        block->SetHostGPR(unrelated, HostRegIndex(23), Imm{0u});

        auto off = allocate(block.get(), false);
        REQUIRE(off->ValueGPR(value).id != 22);
        REQUIRE(off->ValueGPR(read).id != 22);
        auto on = allocate(block.get(), true);
        REQUIRE(on->ValueGPR(value).id == 22);
        REQUIRE(on->ValueGPR(zext).id == 22);
        REQUIRE(on->ValueGPR(read).id == 22);
        REQUIRE(on->IsHostWriteCoalesced(publish->Id()));
        REQUIRE(on->IsHostReadCoalesced(read.Id()));
    }

    SECTION("unknown high half and a crossing write keep the read bridge") {
        IntrusivePtr<Block> unknown{new Block(0, Location{0x8710})};
        auto full = unknown->LoadImm(Imm{swift::u64{0x100000001}})
                            .SetType(ValueType::U64);
        unknown->SetHostGPR(full, HostRegIndex(22), Imm{0u});
        auto read = unknown->GetHostGPR(HostRegIndex(22), Imm{0u})
                           .SetType(ValueType::U32);
        unknown->StoreUniform(Uniform{48, ValueType::U32}, read);
        auto unknown_on = allocate(unknown.get(), true);
        REQUIRE(unknown_on->ValueGPR(read).id != 22);
        REQUIRE_FALSE(unknown_on->IsHostReadCoalesced(read.Id()));

        IntrusivePtr<Block> crossing{new Block(0, Location{0x8720})};
        auto w = crossing->LoadImm(Imm{swift::u32{1}}).SetType(ValueType::U32);
        auto zext = crossing->ZeroExtend32To64(w);
        crossing->SetHostGPR(zext, HostRegIndex(22), Imm{0u});
        auto snapshot = crossing->GetHostGPR(HostRegIndex(22), Imm{0u})
                                .SetType(ValueType::U32);
        auto replacement = crossing->LoadImm(Imm{swift::u64{2}})
                                   .SetType(ValueType::U64);
        crossing->SetHostGPR(replacement, HostRegIndex(22), Imm{0u});
        crossing->StoreUniform(Uniform{56, ValueType::U32}, snapshot);
        auto crossing_on = allocate(crossing.get(), true);
        REQUIRE(crossing_on->ValueGPR(snapshot).id != 22);
        REQUIRE_FALSE(crossing_on->IsHostReadCoalesced(snapshot.Id()));
    }

    SECTION("a U32 GetHostGPR is not accepted as a W-clean publication proof") {
        IntrusivePtr<Block> block{new Block(0, Location{0x8730})};
        auto source = block->GetHostGPR(HostRegIndex(23), Imm{0u})
                              .SetType(ValueType::U32);
        auto zext = block->ZeroExtend32To64(source);
        auto* publish = block->AppendInst(
                OpCode::SetHostGPR, zext, HostRegIndex(22), Imm{0u});
        auto on = allocate(block.get(), true);
        // The older pinned-write chain may choose x22 as the UBFX destination,
        // but that UBFX remains the operation which proves/creates the clean W
        // value. The new pass must not label the whole chain zero-instruction.
        REQUIRE_FALSE(on->IsHostWriteCoalesced(publish->Id()));
    }

    SECTION("emitter removes the proven publication instruction") {
        auto emit_size = [&](bool enabled) {
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
            };
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::ra_coalesce, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{0x8740}, LocationDescriptor{0x8750}, module_config);

            IntrusivePtr<Block> block{new Block(0, Location{0x8740})};
            auto value = block->LoadImm(Imm{swift::u64{0x123456789abcdef0}})
                                 .SetType(ValueType::U64);
            block->SetHostGPR(value, HostRegIndex(22), Imm{0u});
            block->SetTerminal(terminal::ReturnToDispatch{});
            block->ReIdInstr();

            auto features = FeatureSet{};
            features.ra_coalesce = enabled;
            RegAlloc alloc{block->MaxInstrId(), pinned_gprs(), fprs, features};
            RegisterAllocPass::RunForCoalesceTest(block.get(), &alloc, enabled);
            arm64::JitContext context{module, alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(block.get());
            context.Finish();
            return context.CurrentBufferSize();
        };

        const auto off = emit_size(false);
        const auto on = emit_size(true);
        REQUIRE(on + vixl::aarch64::kInstructionSize == off);
    }

    SECTION("expanded scalar producers preserve alias emission and conflict proofs") {
        auto emit_size = [&](OpCode op, ValueType type, swift::u64 location,
                             bool enabled) {
            Config config{
                    .loc_start = 0,
                    .loc_end = 1ull << 48,
                    .enable_jit = true,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
            };
            AddressSpace address_space{config};
            ModuleConfig module_config{};
            module_config.feature_overrides.Set(FeatureId::ra_coalesce, enabled);
            auto module = address_space.MapModule(
                    LocationDescriptor{location}, LocationDescriptor{location + 0x10},
                    module_config);
            auto item = make_expanded_case(op, type, location);
            auto alloc = allocate(item.block.get(), enabled);
            arm64::JitContext context{module, *alloc};
            arm64::JitTranslator translator{context};
            translator.Translate(item.block.get());
            context.Finish();
            return context.CurrentBufferSize();
        };

        std::size_t case_index = 0;
        for (std::size_t index = 0; index < expanded_producers.size(); ++index) {
            const auto [op, name] = expanded_producers[index];
            const std::array types = {ValueType::U32, ValueType::U64};
            for (auto type : types) {
                if (op == OpCode::MulHigh && type == ValueType::U32) {
                    continue;
                }
                CAPTURE(name, type);
                auto item = make_expanded_case(op, type, 0x8800 + case_index * 0x20);
                auto off = allocate(item.block.get(), false);
                REQUIRE(off->ValueGPR(item.produced).id != 22);
                REQUIRE_FALSE(off->IsHostWriteCoalesced(item.publish->Id()));

                auto on = allocate(item.block.get(), true);
                CAPTURE(item.produced.Id(), item.produced.Def()->GetUses(),
                        item.publish->Id(), on->ValueGPR(item.produced).id);
                REQUIRE(on->ValueGPR(item.produced).id == 22);
                REQUIRE(on->IsHostWriteCoalesced(item.publish->Id()));

                auto features = FeatureSet{};
                features.ra_coalesce = true;
                RegAlloc conflict{item.block->MaxInstrId(), pinned_gprs(), fprs, features};
                RegisterAllocPass::RunForCoalesceConflictTest(
                        item.block.get(), &conflict, item.conflict.Id(), 22);
                REQUIRE(conflict.ValueGPR(item.conflict).id == 22);
                REQUIRE_FALSE(conflict.IsHostWriteCoalesced(item.publish->Id()));

                const auto location = 0x9800 + case_index * 0x20;
                const auto off_size = emit_size(op, type, location, false);
                const auto on_size = emit_size(op, type, location, true);
                REQUIRE(on_size + vixl::aarch64::kInstructionSize == off_size);
                ++case_index;
            }
        }
    }
}

TEST_CASE("integer width chains keep X high halves zero for W and X consumers") {
    using namespace swift::x86;

    struct Program {
        std::vector<swift::u8> bytes;
        bool memory_source;
    };
    const std::array programs{
            // mov eax,ecx; add eax,edx; shr eax,3; mov r8,rax; hlt
            Program{{0x89, 0xc8, 0x01, 0xd0, 0xc1, 0xe8, 0x03,
                     0x49, 0x89, 0xc0, 0xf4}, false},
            // mov eax,[rsi]; add eax,edx; shl eax,1; mov r8d,eax;
            // mov r9,r8; hlt. The W consumer must clear r8's old high half
            // before the following X consumer observes it.
            Program{{0x8b, 0x06, 0x01, 0xd0, 0xd1, 0xe0,
                     0x41, 0x89, 0xc0, 0x4d, 0x89, 0xc1, 0xf4}, true},
    };
    const std::array upper_patterns{
            UINT64_C(0x0000000000000000),
            UINT64_C(0xffffffffffffffff),
            UINT64_C(0xa5a5f00d5a5a1234),
    };

    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    void* guest_data = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    REQUIRE(guest_data != MAP_FAILED);
    *reinterpret_cast<swift::u32*>(guest_data) = 0xf13579bdu;

    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    for (std::size_t program_index = 0; program_index < programs.size(); ++program_index) {
        const auto& program = programs[program_index];
        auto* program_code = static_cast<swift::u8*>(guest_code) + program_index * 64;
        std::memcpy(program_code, program.bytes.data(), program.bytes.size());
        for (auto upper : upper_patterns) {
            INFO("memory source=" << program.memory_source << " upper=0x" << std::hex
                                  << upper);
            auto* core = swift::translator::x86::X86Core::Make(instance);
            auto& context = core->GetContext();
            context.rip.qword = reinterpret_cast<VAddr>(program_code);
            context.rax.qword = upper;
            context.rcx.qword = upper ^ UINT64_C(0x0123456789abcdef);
            context.rdx.qword = upper ^ UINT64_C(0xfedcba9876543210);
            context.rsi.qword = reinterpret_cast<VAddr>(guest_data);
            context.r8.qword = upper;
            context.r9.qword = upper;
            core->Run();

            const swift::u32 left = program.memory_source
                    ? *reinterpret_cast<swift::u32*>(guest_data)
                    : static_cast<swift::u32>(upper ^ UINT64_C(0x0123456789abcdef));
            const swift::u32 sum = left +
                    static_cast<swift::u32>(upper ^ UINT64_C(0xfedcba9876543210));
            const swift::u64 expected = program.memory_source
                    ? static_cast<swift::u32>(sum << 1)
                    : static_cast<swift::u32>(sum >> 3);
            REQUIRE(context.rax.qword == expected);
            REQUIRE(context.r8.qword == expected);
            if (program.memory_source) {
                REQUIRE(context.r9.qword == expected);
            }
            swift::translator::x86::X86Core::Destroy(core);
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_data, 4096);
    munmap(guest_code, 4096);
}

TEST_CASE("Single-block register allocation is map-identical to the general path") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // One real block containing all high-risk shapes: fixed host-register
    // aliases (both crossing and non-crossing writes), a host call, a terminal-
    // only value, a BitCast alias, and enough simultaneous scalar liveness to
    // force spills in the deliberately small register pool below.
    HIRBuilder builder{1, true, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1100});

    auto pinned =
            function->GetHostGPR(HostRegIndex(0), Imm{0u}).SetType(ValueType::U64);
    auto snapshot =
            function->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    auto fixed_fpr =
            function->GetHostFPR(HostRegIndex(2), Imm{0u}).SetType(ValueType::V128);

    std::vector<Value> live;
    for (std::uint64_t i = 0; i < 14; ++i) {
        live.push_back(function->LoadImm(Imm{0x100u + i}).SetType(ValueType::U64));
    }
    auto alias = function->BitCast(live[3]).SetType(ValueType::U64);

    // The SetHostGPR crosses snapshot's lifetime, so snapshot must receive a
    // normal allocation instead of aliasing host register 1.
    function->SetHostGPR(live[0], HostRegIndex(1), Imm{0u});
    auto pinned_use = function->Add(pinned, Operand{live[1]}).SetType(ValueType::U64);
    auto snapshot_use =
            function->Add(snapshot, Operand{alias}).SetType(ValueType::U64);

    Params params{};
    params.Push(live[2]);
    params.Push(snapshot_use);
    params.Push(pinned_use);
    function->CallDynamic(Lambda{Imm{std::uint64_t(1ull)}}, params);

    // Consume the pressure values after the call so all of them cross it.
    Value sum = live.back();
    for (int i = static_cast<int>(live.size()) - 2; i >= 0; --i) {
        sum = function->Add(sum, Operand{live[i]}).SetType(ValueType::U64);
    }
    sum = function->Add(sum, Operand{snapshot_use}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    function->StoreUniform(Uniform{16, ValueType::V128}, fixed_fpr);

    auto terminal_cond = function->TestNotZero(live[4]);
    function->EndBlock(terminal::If{
            terminal_cond, terminal::ReturnToDispatch{}, terminal::ReturnToHost{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(function->GetHIRBlocksRPO().size() == 1);

    // Bits set are unavailable. Leave six allocatable GPRs/FPRs, less the
    // default scratch reserve, so the block must take the spill path.
    constexpr std::uint32_t available_gprs = 0x000003f0u;  // x4..x9
    constexpr std::uint32_t available_fprs = 0x00000f78u;  // v3..v6, v8..v11
    GPRSMask gprs{~available_gprs};
    FPRSMask fprs{~available_fprs};
    RegAlloc general{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    RegAlloc fast{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};
    RegAlloc selected{function->MaxInstrCount(), gprs, fprs, FeatureSet{}};

    RegisterAllocPass::Run(function, &general, false, FeatureSet{});
    RegisterAllocPass::Run(function, &fast, true, FeatureSet{});
    RegisterAllocPass::Run(function, &selected, FeatureSet{});

    REQUIRE(general.MapCount() == fast.MapCount());
    bool saw_spill = false;
    for (std::uint32_t id = 0; id < general.MapCount(); ++id) {
        INFO("allocation map id " << id);
        REQUIRE(general.Mapping(id) == fast.Mapping(id));
        saw_spill |= general.Mapping(id).type == RegAlloc::MEM;
    }
    REQUIRE(saw_spill);
    REQUIRE(general.ValueType(pinned) == RegAlloc::GPR);
    REQUIRE(general.ValueGPR(pinned).id == 0);
    REQUIRE(general.ValueType(snapshot) != RegAlloc::REF);
    REQUIRE(general.ValueType(fixed_fpr) == RegAlloc::FPR);
    REQUIRE(general.ValueFPR(fixed_fpr).id == 2);
    const auto& expected = swift::runtime::GetSvmConfig().ra_1blk ? fast : general;
    for (std::uint32_t id = 0; id < selected.MapCount(); ++id) {
        INFO("production selector map id " << id);
        REQUIRE(selected.Mapping(id) == expected.Mapping(id));
    }
}

// Builds a block that keeps `live` scalar values simultaneously live across a
// VecFAdd -- the emitter with the largest scratch appetite in the backend
// (JitTranslator::EmitVecFloatNaNFixup holds eight GPRs at once). Consuming
// the scalars only after the VecFAdd is what makes them live *across* it, so
// the linear scan is asked to fill the register file at exactly the
// instruction that needs the most scratch.
static swift::runtime::ir::Block* BuildScratchPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x1000});
    std::vector<Value> scalars;
    scalars.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        scalars.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // Vector values come from uniforms, the shape the x86 frontend produces
    // for XMM registers (a V128 LoadImm is not a form the backend emits).
    auto lhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
    auto rhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
    auto sum = block->VecFAdd<TypedValue<ValueType::V128>>(lhs, rhs, Imm{32u});
    block->StoreUniform(Uniform{48, ValueType::V128}, sum);
    // Consume the scalars *after* the float op, in reverse definition order.
    Value acc = scalars.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        acc = block->Add(acc, Operand{scalars[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, acc);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Companion to the block above, aimed at the *reload* half of the budget:
// every value is read twice, far apart, so the linear scan spills most of them
// and the second read of each becomes a reload into a scratch register on top
// of the reading opcode's own budget. The final Add names one spilled value
// twice, which costs one register only because JitContext memoizes reloads per
// (instruction, value).
static swift::runtime::ir::Block* BuildReloadPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2000});
    std::vector<Value> values;
    values.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        values.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // First read in reverse order: nothing dies until the very end, so the
    // register file is saturated for the whole block.
    Value first = values.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        first = block->Add(first, Operand{values[i]});
    }
    // Second read of every value, by which point most of them are spilled.
    Value second = values.front();
    for (unsigned i = 1; i < live; i++) {
        second = block->Add(second, Operand{values[i]});
    }
    auto both = block->Add(first, Operand{second});
    // One instruction naming the same value twice, and deliberately the value
    // defined first: it is live across the whole block, so the scan spills it,
    // and both reads become reloads of the same slot. JitContext must serve
    // them from one scratch register -- that memoisation is what the reload
    // accounting above is entitled to assume.
    // And, not Add: its emitter is one of the few that actually spends its
    // whole three-register budget, leaving no slack to absorb a second reload.
    auto doubled = block->And(values.front(), Operand{values.front()});
    auto total = block->Add(both, Operand{doubled});
    block->StoreUniform(Uniform{0, ValueType::U32}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

struct SpillEvictChoiceBlock {
    swift::runtime::ir::Block* block{};
    swift::runtime::ir::Value longest{};
    swift::runtime::ir::Value arriving{};
};

static SpillEvictChoiceBlock BuildSpillEvictChoiceBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2400});
    auto longest = block->LoadImm(Imm{swift::u64{1}});
    auto middle = block->LoadImm(Imm{swift::u64{2}});
    auto shortest = block->LoadImm(Imm{swift::u64{3}});
    auto arriving = block->LoadImm(Imm{swift::u64{4}});
    auto first = block->Add(arriving, Operand{shortest});
    auto second = block->Add(first, Operand{middle});
    auto total = block->Add(second, Operand{longest});
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return {block, longest, arriving};
}

static swift::runtime::ir::Block* BuildSpillEvictFallbackBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2410});
    std::vector<Value> anchors;
    for (swift::u64 value = 1; value <= 7; ++value) {
        anchors.push_back(block->LoadImm(Imm{value}));
    }
    auto arriving = block->LoadImm(Imm{swift::u64{8}});
    auto vector = block->LoadUniform<TypedValue<ValueType::V128>>(
            Uniform{32, ValueType::V128});
    auto converted = block->VecFCvtFloatToInt(
            vector, Imm{32u}, Imm{64u}, Imm{0u}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{40, ValueType::U64}, converted);
    Value total = arriving;
    for (auto it = anchors.rbegin(); it != anchors.rend(); ++it) {
        total = block->Add(total, Operand{*it});
    }
    block->StoreUniform(Uniform{0, ValueType::U64}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Spill eviction chooses farthest end and preserves verified fallback") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    // Six free registers and the ordinary reserve of three leave exactly
    // three value locations. The fourth definition is shorter-lived than all
    // three active values, while `longest` has the unique farthest end.
    const GPRSMask gprs{~((1u << 6) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    auto off_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> off_block{off_case.block};
    RegAlloc off{off_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto off_result = RegisterAllocPass::RunForSpillEvictTest(
            off_case.block, &off, false);
    REQUIRE(off.ValueType(off_case.arriving) == RegAlloc::MEM);
    REQUIRE(off_result.eviction_restarts == 0);

    auto on_case = BuildSpillEvictChoiceBlock();
    swift::runtime::IntrusivePtr<Block> on_block{on_case.block};
    RegAlloc on{on_case.block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    const auto on_result = RegisterAllocPass::RunForSpillEvictTest(
            on_case.block, &on, true);
    REQUIRE(on_result.eviction_restarts >= 1);
    REQUIRE_FALSE(on_result.fell_back_to_ladder);
    REQUIRE(on.ValueType(on_case.longest) == RegAlloc::MEM);
    REQUIRE(on.ValueType(on_case.arriving) == RegAlloc::GPR);

    // This block deliberately reads many long-lived spilled values while the
    // opcode scratch budget is also live. If the default-reserve eviction
    // attempt cannot satisfy Verify(), it must be discarded and the existing
    // reserve ladder must produce the final allocation.
    swift::runtime::IntrusivePtr<Block> fallback_block{BuildSpillEvictFallbackBlock()};
    const GPRSMask fallback_gprs{~((1u << 10) - 1u)};
    RegAlloc fallback{fallback_block->MaxInstrId(), fallback_gprs, fprs,
                      FeatureSet{}};
    const auto fallback_result = RegisterAllocPass::RunForSpillEvictTest(
            fallback_block.get(), &fallback, true);
    REQUIRE(fallback_result.eviction_restarts >= 1);
    REQUIRE(fallback_result.fell_back_to_ladder);
    REQUIRE(fallback_result.final_gpr_reserve > 3);
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
    SUCCEED("AFP FPCR execution probe requires an AArch64 host");
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
    SUCCEED("AFP environment execution probe requires an AArch64 host");
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
    SUCCEED("AFP transparent-helper execution probe requires an AArch64 host");
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

namespace {

enum class AFPShapeOp {
    Add,
    Sub,
    Mul,
    Div,
    Sqrt,
    Fma,
    Min,
    Max,
    Comis,
    Rcp,
    Rsqrt,
};

struct AFPShapeCode {
    std::string text;
    std::size_t instructions{};
};

std::string NormalizeDisasmForCompare(std::string text) {
    constexpr std::string_view kAddressPrefix{"(addr 0x"};
    constexpr std::string_view kReplacement{"(addr <normalized>)"};
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    };

    std::size_t search_from = 0;
    while (true) {
        const auto begin = text.find(kAddressPrefix, search_from);
        if (begin == std::string::npos) {
            break;
        }
        const auto digits_begin = begin + kAddressPrefix.size();
        auto end = digits_begin;
        while (end < text.size() && is_hex(text[end])) {
            ++end;
        }
        if (end == digits_begin || end == text.size() || text[end] != ')') {
            search_from = digits_begin;
            continue;
        }
        text.replace(begin, end - begin + 1, kReplacement);
        search_from = begin + kReplacement.size();
    }
    return text;
}

AFPShapeCode CompileAFPShape(AFPShapeOp op,
                             swift::u32 lane_bits,
                             bool scalar,
                             bool scalar_insert,
                             bool afp_nan) {
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
            .sse_scalar_insert = scalar_insert,
            .sse_afp_nan = afp_nan,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x2d00})};
    auto left = block->LoadUniform(Uniform{0, ValueType::V128})
                        .SetType(ValueType::V128);
    auto right = block->LoadUniform(Uniform{16, ValueType::V128})
                         .SetType(ValueType::V128);
    auto third = block->LoadUniform(Uniform{32, ValueType::V128})
                         .SetType(ValueType::V128);
    Value result;
    switch (op) {
        case AFPShapeOp::Add:
            if (!scalar) {
                result = block->VecFAdd(left, right, Imm{swift::u32{lane_bits}});
            } else if (lane_bits == 32) {
                result = block->VecFAddScalar32(left, right);
            } else {
                result = block->VecFAddScalar64(left, right);
            }
            break;
        case AFPShapeOp::Sub:
            if (!scalar) {
                result = block->VecFSub(left, right, Imm{swift::u32{lane_bits}});
            } else if (lane_bits == 32) {
                result = block->VecFSubScalar32(left, right);
            } else {
                result = block->VecFSubScalar64(left, right);
            }
            break;
        case AFPShapeOp::Mul:
            if (!scalar) {
                result = block->VecFMul(left, right, Imm{swift::u32{lane_bits}});
            } else if (lane_bits == 32) {
                result = block->VecFMulScalar32(left, right);
            } else {
                result = block->VecFMulScalar64(left, right);
            }
            break;
        case AFPShapeOp::Div:
            if (!scalar) {
                result = block->VecFDiv(left, right, Imm{swift::u32{lane_bits}});
            } else if (lane_bits == 32) {
                result = block->VecFDivScalar32(left, right);
            } else {
                result = block->VecFDivScalar64(left, right);
            }
            break;
        case AFPShapeOp::Sqrt:
            result = block->VecFUnary(left,
                                      left,
                                      Imm{swift::u32{lane_bits}},
                                      Imm{swift::u32{0}},
                                      Imm{swift::u32{scalar ? 1u : 0u}});
            break;
        case AFPShapeOp::Fma:
            result = block->VecFMulAdd(left,
                                       right,
                                       third,
                                       Imm{swift::u32{lane_bits}},
                                       Imm{swift::u32{0}});
            break;
        case AFPShapeOp::Min:
        case AFPShapeOp::Max:
            result = block->VecFMinMax(left,
                                       right,
                                       Imm{swift::u32{lane_bits}},
                                       Imm{swift::u32{op == AFPShapeOp::Max ? 1u : 0u}},
                                       Imm{swift::u32{scalar ? 1u : 0u}});
            break;
        case AFPShapeOp::Comis: {
            auto flags = block->VecFCmp(left,
                                        right,
                                        Imm{swift::u32{lane_bits}},
                                        Imm{swift::u32{0}})
                                 .SetType(ValueType::U64);
            block->StoreUniform(Uniform{48, ValueType::U64}, flags);
            break;
        }
        case AFPShapeOp::Rcp:
        case AFPShapeOp::Rsqrt:
            result = block->VecFUnary(
                    left,
                    left,
                    Imm{swift::u32{lane_bits}},
                    Imm{swift::u32{op == AFPShapeOp::Rcp ? 1u : 2u}},
                    Imm{swift::u32{scalar ? 1u : 0u}});
            break;
    }
    if (op != AFPShapeOp::Comis) {
        block->StoreUniform(Uniform{48, ValueType::V128},
                            result.SetType(ValueType::V128));
    }
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();

    RegAlloc reg_alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(),
                       FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, scalar_insert, FeatureSet{});
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
    AFPShapeCode code;
    for (const auto* instruction = first; instruction < last;
         instruction = instruction->GetNextInstruction()) {
        decoder.Decode(instruction);
        code.text += disassembler.GetOutput();
        code.text += '\n';
        ++code.instructions;
    }
    return code;
}

const char* AFPShapeName(AFPShapeOp op) {
    switch (op) {
        case AFPShapeOp::Add: return "add";
        case AFPShapeOp::Sub: return "sub";
        case AFPShapeOp::Mul: return "mul";
        case AFPShapeOp::Div: return "div";
        case AFPShapeOp::Sqrt: return "sqrt";
        case AFPShapeOp::Fma: return "fma";
        case AFPShapeOp::Min: return "min";
        case AFPShapeOp::Max: return "max";
        case AFPShapeOp::Comis: return "comis";
        case AFPShapeOp::Rcp: return "rcp";
        case AFPShapeOp::Rsqrt: return "rsqrt";
    }
    return "unknown";
}

}  // namespace

TEST_CASE("AFP P1 removes guards only for the arithmetic allowlist") {
    constexpr std::array included{
            AFPShapeOp::Add,
            AFPShapeOp::Sub,
            AFPShapeOp::Mul,
            AFPShapeOp::Div,
            AFPShapeOp::Sqrt,
    };
    for (const auto op : included) {
        for (const swift::u32 lane_bits : {swift::u32{32}, swift::u32{64}}) {
            for (const bool scalar : {false, true}) {
                for (const bool scalar_insert : {false, true}) {
                    if (!scalar && scalar_insert) continue;
                    INFO(AFPShapeName(op) << " f" << lane_bits
                                          << (scalar ? " scalar" : " packed")
                                          << (scalar_insert ? " tied" : " legacy"));
                    const auto off = CompileAFPShape(
                            op, lane_bits, scalar, scalar_insert, false);
                    const auto on = CompileAFPShape(
                            op, lane_bits, scalar, scalar_insert, true);
                    INFO("OFF:\n" << off.text << "ON:\n" << on.text);
                    REQUIRE(on.instructions < off.instructions);
                    if (scalar) {
                        REQUIRE(off.text.find("b.vs") != std::string::npos);
                        REQUIRE(on.text.find("b.vs") == std::string::npos);
                    } else if (lane_bits == 32) {
                        REQUIRE(off.text.find("uminv") != std::string::npos);
                        REQUIRE(on.text.find("uminv") == std::string::npos);
                    } else {
                        REQUIRE(off.text.find("cbz") != std::string::npos);
                        REQUIRE(on.text.find("cbz") == std::string::npos);
                    }
                }
            }
        }
    }
}

TEST_CASE("AFP P1 leaves excluded FP opcode lowering unchanged") {
    REQUIRE(NormalizeDisasmForCompare(
                    "b.mi #+0x8 (addr 0xaA09)\nmov x0, #0x123") ==
            "b.mi #+0x8 (addr <normalized>)\nmov x0, #0x123");

    struct ExcludedShape {
        AFPShapeOp op;
        swift::u32 lane_bits;
        bool scalar;
    };
    constexpr std::array excluded{
            ExcludedShape{AFPShapeOp::Fma, 32, false},
            ExcludedShape{AFPShapeOp::Fma, 64, false},
            ExcludedShape{AFPShapeOp::Min, 32, false},
            ExcludedShape{AFPShapeOp::Min, 64, true},
            ExcludedShape{AFPShapeOp::Max, 32, true},
            ExcludedShape{AFPShapeOp::Max, 64, false},
            ExcludedShape{AFPShapeOp::Comis, 32, true},
            ExcludedShape{AFPShapeOp::Comis, 64, true},
            ExcludedShape{AFPShapeOp::Rcp, 32, false},
            ExcludedShape{AFPShapeOp::Rcp, 32, true},
            ExcludedShape{AFPShapeOp::Rsqrt, 32, false},
            ExcludedShape{AFPShapeOp::Rsqrt, 32, true},
    };
    for (const auto& shape : excluded) {
        for (const bool scalar_insert : {false, true}) {
            if (!shape.scalar && scalar_insert) continue;
            INFO(AFPShapeName(shape.op) << " f" << shape.lane_bits
                                        << (shape.scalar ? " scalar" : " packed")
                                        << (scalar_insert ? " tied" : " legacy"));
            const auto off = CompileAFPShape(shape.op,
                                             shape.lane_bits,
                                             shape.scalar,
                                             scalar_insert,
                                             false);
            const auto on = CompileAFPShape(shape.op,
                                            shape.lane_bits,
                                            shape.scalar,
                                            scalar_insert,
                                            true);
            INFO("OFF:\n" << off.text << "ON:\n" << on.text);
            REQUIRE(on.instructions == off.instructions);
            REQUIRE(NormalizeDisasmForCompare(on.text) ==
                    NormalizeDisasmForCompare(off.text));
        }
    }
}

TEST_CASE("Scratch pool survives a register file saturated across a VecFAdd") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: JitContext::GetTmpX hands out registers the
    // allocation pass left free, and never releases them before the next
    // instruction. EmitVecFAdd needs eight at once. With a fixed reserve
    // (4 before this change) the scan was free to fill the register file down
    // to four, so a VecFAdd in a saturated block either panicked with
    // "No free temporary GPR" or -- worse, when the pool happened to have room
    // -- silently handed the NaN fixup a register holding a live value.
    //
    // Saturating the file is the whole point, so `live` is deliberately larger
    // than any ARM64 GPR pool.
    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();
#if defined(__linux__) && !defined(__ANDROID__)
    // Trampoline construction must not tax every unit: x18 starts in the
    // ordinary dynamic pool and is removed only after a unit proves it spills.
    REQUIRE_FALSE(gprs.Get(18));
#endif

#if defined(__linux__) && !defined(__ANDROID__)
    bool saw_conditional_spill_unit = false;
#endif
    auto check_and_emit = [&](Block* raw, int expected_spill = -1) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});
#if defined(__linux__) && !defined(__ANDROID__)
        const bool has_spill = reg_alloc.SpillCount() != 0;
        saw_conditional_spill_unit |= has_spill;
        if (expected_spill >= 0) {
            REQUIRE(has_spill == (expected_spill != 0));
        }
        // This is the mask JitContext seeds into every instruction's dirty
        // baseline. Spilling units exclude x18; zero-spill units retain the
        // exact trampoline pool and may receive x18 from ordinary GetTmpX.
        REQUIRE(reg_alloc.GetGprs().Get(18) == has_spill);
#endif

        // 1. The allocation must leave every instruction the scratch its
        //    emitter is declared to need, plus a reload register for each
        //    DISTINCT spilled value it names (JitContext reloads a value once
        //    per instruction however often the instruction names it). This is
        //    the contract GetTmpX relies on.
        for (auto& inst : block->GetInstList()) {
            auto need = ScratchBudget(inst, FeatureSet{});
            unsigned reloads_gpr = 0, reloads_fpr = 0;
            std::vector<std::uint32_t> counted;
            auto count = [&](const Value& value) {
                if (!value.Defined() || reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    return;
                }
                if (std::find(counted.begin(), counted.end(), value.Id()) != counted.end()) {
                    return;
                }
                counted.push_back(value.Id());
                auto type = value.Type();
                (type >= ValueType::V8 && type <= ValueType::V256 ? reloads_fpr : reloads_gpr)++;
            };
            for (auto& value : inst.GetValues()) {
                count(value);
            }
            if (inst.HasValue()) {
                count(Value{&inst});
            }
            INFO("opcode " << static_cast<unsigned>(inst.GetOp()) << " at id " << inst.Id());
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyGPR(inst.Id()).GetClearCount()) >=
                    need.gpr + reloads_gpr);
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyFPR(inst.Id()).GetClearCount()) >=
                    need.fpr + reloads_fpr);
        }

        // 2. And the JIT must actually emit it. GetTmpX panics through
        //    AssertFailed, so reaching the end of Translate is the assertion:
        //    a scratch shortfall aborts here rather than returning.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    };

#if defined(__linux__) && !defined(__ANDROID__)
    auto* no_spill = new Block(0, Location{0x1800});
    const auto immediate = no_spill->LoadImm(Imm{7u});
    no_spill->StoreUniform(Uniform{0, ValueType::U32}, immediate);
    no_spill->SetTerminal(terminal::ReturnToDispatch{});
    no_spill->ReIdInstr();
    check_and_emit(no_spill, 0);
#endif

    for (unsigned live : {8u, 16u, 24u, 40u}) {
        INFO("live scalar values across the VecFAdd: " << live);
        check_and_emit(BuildScratchPressureBlock(live));
    }
    // Reload pressure: values read twice across a saturated file, and one
    // instruction naming the same spilled value twice. Capped below the point
    // where the 64-slot State::spill_area runs out -- an unrelated limit
    // (backend::kMaxSpillSlots) with its own loud assert.
    for (unsigned live : {12u, 20u, 28u}) {
        INFO("values read twice across a saturated file: " << live);
        check_and_emit(BuildReloadPressureBlock(live));
    }

#if defined(__linux__) && !defined(__ANDROID__)
    REQUIRE(saw_conditional_spill_unit);
#endif
}

TEST_CASE("Add Sub precise scratch prices cover emitted peaks") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    enum class RightShape { Reg, LargeImm, Shift, Composite };
    auto make_price = [](ValueType type, Flags flags, bool branch_only,
                         RightShape shape) {
        Block block{0, Location{0x1d00}};
        auto left = block.LoadImm(Imm{7u}).SetType(type);
        auto other = block.LoadImm(Imm{3u}).SetType(type);
        Operand right{other};
        switch (shape) {
            case RightShape::Reg:
                break;
            case RightShape::LargeImm:
                right = Operand{Imm{swift::u64{0x123456789}}};
                break;
            case RightShape::Shift:
                right = Operand{other, Imm{1u}, OperandLsl};
                break;
            case RightShape::Composite:
                right = Operand{left, other, OperandPlus};
                break;
        }
        auto result = block.Add(left, right).SetType(type);
        if (branch_only) {
            block.AppendInst(OpCode::BranchOnlyFlags, result, flags);
        } else if (flags != Flags::None) {
            block.SaveFlags(result, flags);
        }
        return PreciseAddSubScratchBudget(*result.Def()).gpr;
    };

    REQUIRE(make_price(ValueType::U64, Flags::None, false, RightShape::Reg) == 0);
    REQUIRE(make_price(ValueType::U64, Flags::Parity, false, RightShape::Reg) == 0);
    REQUIRE(make_price(ValueType::U64, Flags::AuxiliaryCarry, false, RightShape::Reg) == 1);
    REQUIRE(make_price(ValueType::U64, Flags::NZCV, false, RightShape::Reg) == 1);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Reg) == 2);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::LargeImm) == 3);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Shift) == 2);
    REQUIRE(make_price(ValueType::U64, Flags::All, false, RightShape::Composite) == 3);
    REQUIRE(make_price(ValueType::U64, Flags::All, true, RightShape::Composite) == 1);
    REQUIRE(make_price(ValueType::U8, Flags::All, false, RightShape::Reg) == 3);
    REQUIRE(make_price(ValueType::U16, Flags::All, false, RightShape::Composite) == 4);
    REQUIRE(make_price(ValueType::U8, Flags::All, true, RightShape::Composite) == 2);
    REQUIRE(make_price(ValueType::U16, Flags::AuxiliaryCarry, false,
                       RightShape::Composite) == 2);

    auto make_host_price = [](ValueType type, Flags flags) {
        Block block{0, Location{0x1d80}};
        auto left = block.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(type);
        auto result = block.Add(left, Operand{left}).SetType(type);
        if (flags != Flags::None) block.SaveFlags(result, flags);
        return PreciseAddSubScratchBudget(*result.Def()).gpr;
    };
    REQUIRE(make_host_price(ValueType::U16, Flags::None) == 2);
    REQUIRE(make_host_price(ValueType::U16, Flags::All) == 5);
    REQUIRE(make_host_price(ValueType::U32, Flags::All) == 2);

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    Block allocation_contract{0, Location{0x1dc0}};
    auto contract_left =
            allocation_contract.LoadImm(Imm{7u}).SetType(ValueType::U16);
    auto contract_right =
            allocation_contract.LoadImm(Imm{3u}).SetType(ValueType::U16);
    auto contract_result = allocation_contract
                                   .Add(contract_left, Operand{contract_right})
                                   .SetType(ValueType::U16);
    allocation_contract.SaveFlags(contract_result, Flags::All);
    allocation_contract.ReIdInstr();
    RegAlloc contract_alloc{allocation_contract.MaxInstrId(), gprs, fprs,
                            FeatureSet{}};
    RegisterAllocPass::Run(&allocation_contract, &contract_alloc, false, FeatureSet{});
    REQUIRE(contract_alloc.ValueType(contract_left) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueType(contract_right) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueType(contract_result) == RegAlloc::GPR);
    REQUIRE(contract_alloc.ValueGPR(contract_result).id !=
            contract_alloc.ValueGPR(contract_left).id);
    REQUIRE(contract_alloc.ValueGPR(contract_result).id !=
            contract_alloc.ValueGPR(contract_right).id);

    auto measure = [&](ValueType type, Flags flags, bool branch_only,
                       RightShape shape, bool host_read = false) {
        Block block{0, Location{0x1e00}};
        Value left;
        Value other;
        if (host_read) {
            left = block.GetHostGPR(HostRegIndex(22), Imm{0u}).SetType(type);
            other = block.GetHostGPR(HostRegIndex(23), Imm{0u}).SetType(type);
        } else {
            left = block.LoadImm(Imm{7u}).SetType(type);
            other = block.LoadImm(Imm{3u}).SetType(type);
        }
        auto pending = block.Add(left, Operand{other}).SetType(ValueType::U64);
        block.SaveFlags(pending, Flags::All);
        Operand right{left};
        switch (shape) {
            case RightShape::Reg:
                break;
            case RightShape::LargeImm:
                right = Operand{Imm{swift::u64{0x123456789}}};
                break;
            case RightShape::Shift:
                right = Operand{left, Imm{1u}, OperandLsl};
                break;
            case RightShape::Composite:
                right = Operand{left, other, OperandPlus};
                break;
        }
        auto target = block.Add(left, right).SetType(type);
        if (branch_only) {
            block.AppendInst(OpCode::BranchOnlyFlags, target, flags);
        } else if (flags != Flags::None) {
            block.SaveFlags(target, flags);
        }
        block.ReIdInstr();

        RegAlloc alloc{block.MaxInstrId(), gprs, fprs, FeatureSet{}};
        alloc.MapRegister(left.Id(), HostGPR{22});
        alloc.MapRegister(other.Id(), HostGPR{23});
        alloc.MapRegister(pending.Id(), HostGPR{24});
        // Current RA does not tie narrow Add/Sub destinations to either input.
        // Keep the emitter measurement on that allocation contract.
        alloc.MapRegister(target.Id(), HostGPR{10});
        auto active_gprs = gprs;
        auto active_fprs = fprs;
        alloc.SetActiveRegs(pending.Id(), active_gprs, active_fprs);
        alloc.SetActiveRegs(target.Id(), active_gprs, active_fprs);

        arm64::JitContext context{module, alloc};
        arm64::JitTranslator translator{context};
        context.SetCurrent(&block);
        context.TickIR(pending.Def());
        translator.EmitAdd(pending.Def());
        context.EndInstructionScratch();
        context.TickIR(target.Def());
        // Measure independently of either OFF's legacy cap or ON's proposed
        // price. Terminal scratch supplies a seven-register observation
        // envelope but otherwise uses the same per-instruction masks.
        context.BeginTerminalScratch();
        translator.EmitAdd(target.Def());
        context.EndTerminalScratch();
        const swift::u32 peak = context.LastInstructionScratchGPR();
        const swift::u32 price = PreciseAddSubScratchBudget(*target.Def()).gpr;
        INFO("type " << static_cast<unsigned>(type) << " flags "
                     << static_cast<unsigned long long>(flags) << " branch "
                     << branch_only << " shape " << static_cast<unsigned>(shape)
                     << " peak " << peak << " price " << price);
        REQUIRE(peak <= price);
        context.Finish();
        return peak;
    };

    REQUIRE(measure(ValueType::U64, Flags::None, false, RightShape::Reg) == 0);
    REQUIRE(measure(ValueType::U64, Flags::All, false, RightShape::LargeImm) == 3);
    REQUIRE(measure(ValueType::U64, Flags::All, false, RightShape::Composite) == 3);
    REQUIRE(measure(ValueType::U64, Flags::All, true, RightShape::Composite) == 1);
    REQUIRE(measure(ValueType::U8, Flags::All, false, RightShape::Reg) == 3);
    REQUIRE(measure(ValueType::U16, Flags::All, false, RightShape::Composite) == 4);
    REQUIRE(measure(ValueType::U8, Flags::All, true, RightShape::Composite) == 2);
    REQUIRE(measure(ValueType::U16, Flags::None, false, RightShape::Reg,
                    true) == 2);
    REQUIRE(measure(ValueType::U16, Flags::All, false, RightShape::Reg,
                    true) == 5);
}

// --- spill-slot recycling ----------------------------------------------------
//
// The blocks below pin the register file full with long-lived "anchor" values
// and then push a long stream of SHORT-lived values past it. Every short value
// spills (the file is full) but at most two are live at any instant, so the
// number of stack slots the unit needs is a small constant while the number of
// spill *events* is proportional to the churn. That difference is the whole
// point: a linear scan that never returns a slot consumes one per event and
// dies on backend::kMaxSpillSlots; one that recycles stays flat.

static swift::runtime::ir::Block* BuildSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x3000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // The churn. `acc` is a chain: acc(i) dies at exactly the instruction that
    // defines acc(i+1), which is the boundary case an off-by-one in the expiry
    // test (`end < start` vs `end <= start`) gets wrong -- both are live at
    // that instruction, so they must not share a slot.
    Value acc = block->LoadImm(Imm{0u});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadImm(Imm{static_cast<std::uint32_t>(0x1000 + i)});
        acc = block->Add(acc, Operand{tmp});
    }
    // Anchors are consumed only here, so they are live across the whole churn.
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->Add(sum, Operand{held[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(sum, Operand{acc}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Same shape in the vector file. A spilled V128 occupies TWO consecutive u64
// slots and is accessed with a 16-byte Ldr/Str, so recycling it has to return
// both halves and keep the surviving pairs even-aligned (the scaled offset form
// cannot encode an odd one).
static swift::runtime::ir::Block* BuildVecSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x4000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        acc = block->VecAdd<TypedValue<ValueType::V128>>(acc, tmp, Imm{32u});
    }
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->VecAdd<TypedValue<ValueType::V128>>(sum, held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(sum, acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Both files churning at once, so scalar and SIMD spills interleave over the
// same recycled stack: this is the case where returning only one half of a
// SIMD pair, or losing the even alignment, shows up.
static swift::runtime::ir::Block* BuildMixedSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x5000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value gpr_acc = block->LoadImm(Imm{0u});
    Value vec_acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto scalar = block->LoadImm(Imm{static_cast<std::uint32_t>(0x2000 + i)});
        gpr_acc = block->Add(gpr_acc, Operand{scalar});
        auto vec = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        vec_acc = block->VecAdd<TypedValue<ValueType::V128>>(vec_acc, vec, Imm{32u});
    }
    Value gpr_sum = gpr_held.back();
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(gpr_sum, Operand{gpr_acc}));
    block->StoreUniform(Uniform{16, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Manufactures the one stack layout that tells an even-aligned pair search from
// a naive one: a scalar sitting on the low half of a freed SIMD pair, with the
// high half and the pair above it free. The free slots are then {odd, even,
// even+1} and the first *adjacent free pair* starts on an ODD index -- which a
// 16-byte Ldr/Str cannot address with the scaled offset form. Only a search
// that steps by two rejects it.
static swift::runtime::ir::Block* BuildPairFragmentBlock(unsigned gpr_anchors,
                                                         unsigned vec_anchors,
                                                         unsigned rounds) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x6000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < gpr_anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    for (unsigned i = 0; i < vec_anchors; i++) {
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    for (unsigned r = 0; r < rounds; r++) {
        auto va = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
        auto vb = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
        block->StoreUniform(Uniform{64, ValueType::V128}, va);  // va dies
        auto sx = block->LoadImm(Imm{static_cast<std::uint32_t>(0x30 + r)});
        block->StoreUniform(Uniform{80, ValueType::V128}, vb);  // vb dies; sx now holds va's low slot
        auto vc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{48, ValueType::V128});
        block->StoreUniform(Uniform{96, ValueType::V128}, vc);
        block->StoreUniform(Uniform{0, ValueType::U32}, sx);
    }
    Value gpr_sum = gpr_held.back();
    for (int i = static_cast<int>(gpr_anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
    }
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(vec_anchors) - 2; i >= 0; i--) {
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, gpr_sum);
    block->StoreUniform(Uniform{112, ValueType::V128}, vec_sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Spill slots are recycled, not merely handed out") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: SpillAtInterval never recorded its interval
    // in the allocator's active set, so the MEM arm of ExpireOldIntervals --
    // and FreeSpill with it -- was unreachable. Slots were handed out and never
    // returned, making the 64-slot State::spill_area a budget for the TOTAL
    // number of spills in a compilation unit instead of the number live at
    // once. Any long enough block of short-lived spills aborted the guest with
    // "spill area exhausted".
    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();
    // Filling the file exactly means the anchors take every allocatable
    // register and only the scratch reserve's worth spills permanently, so the
    // slot high-water mark is dominated by the churn -- which is what is under
    // test.
    const auto gpr_pool = static_cast<unsigned>(GPRSMask{gprs}.GetClearCount());
    const auto fpr_pool = static_cast<unsigned>(FPRSMask{fprs}.GetClearCount());

    // Live range of a value as the linear scan models it in block mode: from
    // its defining instruction to the last instruction that names it, extended
    // to the end of the block when the terminal reads it.
    auto live_ranges = [](Block* block) {
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> range;
        for (auto& inst : block->GetInstList()) {
            if (inst.HasValue()) {
                range[inst.Id()] = {inst.Id(), inst.Id()};
            }
        }
        for (auto& inst : block->GetInstList()) {
            for (auto& value : inst.GetValues()) {
                if (!value.Defined()) continue;
                auto it = range.find(value.Id());
                if (it != range.end()) {
                    it->second.second = std::max<std::uint32_t>(it->second.second, inst.Id());
                }
            }
        }
        return range;
    };

    auto check_recycling = [&](Block* raw, unsigned expect_min_spills) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
        // Pre-fix this call itself threw ("spill area exhausted: slot 64 (+1)
        // >= 64 reserved slots") long before any assertion below could run.
        // Select the historical spill-current policy explicitly: this test's
        // pressure generator relies on hundreds of churn spills, while the
        // farthest-end experiment deliberately removes most of them.
        (void)RegisterAllocPass::RunForSpillEvictTest(
                block.get(), &reg_alloc, false);

        auto ranges = live_ranges(block.get());
        struct Occupancy {
            std::uint32_t id;
            std::uint32_t slot;
            std::uint32_t width;
            std::uint32_t start;
            std::uint32_t end;
        };
        std::vector<Occupancy> spilled;
        for (auto& inst : block->GetInstList()) {
            // A bitcast is an alias of its source, not an allocation of its
            // own; counting it would report one slot with two owners.
            if (!inst.HasValue() || inst.IsBitCastOperation()) {
                continue;
            }
            Value value{&inst};
            if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                continue;
            }
            const auto type = inst.ReturnType();
            const bool is_vector = type >= ValueType::V8 && type <= ValueType::V256;
            const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
            auto& r = ranges[inst.Id()];
            spilled.push_back({inst.Id(), slot, is_vector ? 2u : 1u, r.first, r.second});
        }

        // 1. The unit must actually have gone down the spill path, hard.
        INFO("spilled values: " << spilled.size());
        REQUIRE(spilled.size() >= expect_min_spills);

        std::uint32_t high_water = 0;
        for (auto& s : spilled) {
            high_water = std::max(high_water, s.slot + s.width);
            // 2. A 16-byte Ldr/Str encodes only a multiple-of-16 offset, so a
            //    SIMD pair must stay even-aligned however the stack is reused.
            if (s.width == 2) {
                INFO("SIMD value " << s.id << " at slot " << s.slot);
                REQUIRE(s.slot % 2 == 0);
            }
            // 3. Never past the reservation: beyond it the Str walks into the
            //    uniform buffer that follows State::spill_area.
            REQUIRE(s.slot + s.width <= kMaxSpillSlots);
        }

        // 4. Two values that are live at the same time must not share a slot.
        //    This is what makes recycling safe rather than merely cheap: a slot
        //    returned one instruction too early is handed to an overlapping
        //    value and both Ldr/Str target the same address.
        for (std::size_t i = 0; i < spilled.size(); i++) {
            for (std::size_t j = i + 1; j < spilled.size(); j++) {
                const auto& a = spilled[i];
                const auto& b = spilled[j];
                const bool ranges_overlap = a.start <= b.end && b.start <= a.end;
                if (!ranges_overlap) {
                    continue;
                }
                const bool slots_overlap =
                        a.slot < b.slot + b.width && b.slot < a.slot + a.width;
                INFO("values " << a.id << " [" << a.start << "," << a.end << "] slot " << a.slot
                               << "+" << a.width << " and " << b.id << " [" << b.start << ","
                               << b.end << "] slot " << b.slot << "+" << b.width);
                REQUIRE_FALSE(slots_overlap);
            }
        }

        // 5. And the reuse must be real: far more spills than slots. Without
        //    recycling these are equal by construction.
        INFO("high water " << high_water << " slots for " << spilled.size() << " spills");
        REQUIRE(high_water * 2 < spilled.size());

        // 6. Finally the JIT has to emit it. GetTmpX/GetTmpV and
        //    SpillGPR/SpillFPR assert through AssertFailed, so reaching the end
        //    of Translate is itself the assertion.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
        return high_water;
    };

    SECTION("scalar churn") {
        // 200 rounds of (LoadImm, Add) past a full file: >=400 spill events
        // against a 64-slot area. Pre-fix this aborts at the 64th.
        check_recycling(BuildSpillChurnBlock(gpr_pool, 200), 300);
    }
    SECTION("vector churn") {
        check_recycling(BuildVecSpillChurnBlock(fpr_pool, 200), 300);
    }
    SECTION("mixed churn") {
        // Scalar and SIMD spills interleaved over one recycled stack.
        check_recycling(BuildMixedSpillChurnBlock(std::min(gpr_pool, fpr_pool), 200), 500);
    }
    SECTION("fragmented pairs stay 16-byte aligned") {
        // Deliberately leaves the first adjacent free pair on an ODD index.
        check_recycling(BuildPairFragmentBlock(gpr_pool, fpr_pool, 120), 400);
    }
}

// --- Peeled GetOperand must retain its address register ----------------------
//
// SVM_MEM_NARROW_FUSE originally suppressed a single-use identity GetOperand
// in the emitter and made the following memory operation read the wrapper's
// source directly.  Register allocation still considered that source dead at
// GetOperand, however, and could reuse its register for a StoreMemory value:
//
//   LoadUniform address -> x0
//   GetOperand(address)  -> x1   (suppressed)
//   LoadUniform value    -> x0
//   StoreMemory          -> str x0, [x0]  // value used as its own address
//
// The optimized form is valid only when RA transfers the source register to
// the GetOperand result, whose interval remains live through StoreMemory.
TEST_CASE("peeled GetOperand keeps its address live through the memory use") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    auto* raw_block = new Block(0, Location{0x6f00});
    IntrusivePtr<Block> block{raw_block};
    auto address =
            raw_block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto memory_address =
            raw_block->GetOperand(Operand{address}).SetType(ValueType::U64);
    auto value =
            raw_block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    raw_block->StoreMemory(Operand{memory_address}, value);
    raw_block->SetTerminal(terminal::ReturnToDispatch{});
    raw_block->ReIdInstr();

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    RegAlloc reg_alloc{raw_block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(),
                       FeatureSet{}};
    RegisterAllocPass::Run(raw_block, &reg_alloc, false, FeatureSet{});

    const bool enabled = swift::runtime::GetSvmConfig().mem_narrow_fuse;
    if (enabled) {
        REQUIRE(reg_alloc.ValueType(address) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueType(memory_address) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueType(value) == RegAlloc::GPR);
        REQUIRE(reg_alloc.ValueGPR(address).id ==
                reg_alloc.ValueGPR(memory_address).id);
        REQUIRE(reg_alloc.ValueGPR(memory_address).id !=
                reg_alloc.ValueGPR(value).id);
    }

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(raw_block);
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);
}

TEST_CASE("address EA tie transfers a terminal fixed alias") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    auto* raw = new Block(0, Location{0x6f20});
    IntrusivePtr<Block> block{raw};
    auto source = raw->GetHostGPR(HostRegIndex(6), Imm{0u}).SetType(ValueType::U64);
    auto address = raw->GetOperand(Operand{source}).SetType(ValueType::U64);
    auto value = raw->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    raw->StoreMemory(Operand{address}, value);
    raw->SetTerminal(terminal::ReturnToDispatch{});
    raw->ReIdInstr();

    // x6 模拟静态映射的固定家；普通线性扫描不能把其它值分配到它。
    const GPRSMask gprs{~((1u << 19) - 1u) | (1u << 6)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{raw->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(raw, &alloc, false, FeatureSet{});

    const bool enabled = swift::runtime::GetSvmConfig().addr_ea_tie;
    REQUIRE(alloc.ValueGPR(source).id == 6);
    REQUIRE((alloc.ValueGPR(address).id == alloc.ValueGPR(source).id) == enabled);
    REQUIRE(alloc.ValueGPR(address).id != alloc.ValueGPR(value).id);
}

TEST_CASE("composite memory EA survives only in identity mode") {
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

    auto decode_address = [&](std::array<swift::u8, 5> bytes, bool identity) {
        const auto pc = reinterpret_cast<VAddr>(bytes.data());
        auto* raw = new Block(0, Location{pc});
        IntrusivePtr<Block> block{raw};
        Assembler assembler{raw};
        X64Decoder decoder{pc,
                           &memory,
                           &assembler,
                           true,
                           Arm64Features::None,
                           false,
                           identity,
                           FeatureSet{}};
        decoder.Decode();
        for (auto& inst : raw->GetInstList()) {
            if (inst.GetOp() == OpCode::LoadMemory) {
                return std::pair{block, inst.GetArg<Operand>(0)};
            }
        }
        FAIL("memory load was not decoded");
        return std::pair{block, Operand{}};
    };

    const bool enabled = swift::runtime::GetSvmConfig().addr_ea_tie;
    // mov eax,[rbx+8]; hlt。末尾补零只用于固定数组长度。
    auto [identity_imm_block, identity_imm] =
            decode_address({0x8b, 0x43, 0x08, 0xf4, 0x00}, true);
    auto [bias_imm_block, bias_imm] =
            decode_address({0x8b, 0x43, 0x08, 0xf4, 0x00}, false);
    // mov eax,[rbx+rcx*4]; hlt。
    auto [identity_ext_block, identity_ext] =
            decode_address({0x8b, 0x04, 0x8b, 0xf4, 0x00}, true);

    REQUIRE((!identity_imm.GetRight().Null()) == enabled);
    if (enabled) {
        REQUIRE(identity_imm.GetOp() == OperandOp::Plus);
        REQUIRE(identity_imm.GetRight().IsImm());
        REQUIRE(identity_ext.GetOp() == OperandOp::PlusExt);
        REQUIRE(identity_ext.GetRight().IsValue());
    } else {
        REQUIRE(identity_ext.GetRight().Null());
    }
    // bias 不论开关状态都保留旧的 GetOperand 单值边界。
    REQUIRE(bias_imm.GetRight().Null());
    REQUIRE(bias_imm.GetLeft().IsValue());
    REQUIRE(bias_imm.GetLeft().value.Def()->GetOp() == OpCode::GetOperand);
}

TEST_CASE("absolute GetOperand materializes directly into its result") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
    };
    AddressSpace address_space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x7800})};
    auto address =
            block->GetOperand(Operand{Imm{swift::u64{0x004c7e10}}})
                    .SetType(ValueType::U64);
    block->ReIdInstr();

    const GPRSMask gprs{~((1u << 8) - 1u)};
    const FPRSMask fprs{~((1u << 8) - 1u)};
    RegAlloc alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    alloc.MapRegister(address.Id(), HostGPR{6});
    auto active_gprs = gprs;
    auto active_fprs = fprs;
    active_gprs.Mark(6);
    alloc.SetActiveRegs(address.Id(), active_gprs, active_fprs);

    arm64::JitContext context{address_space.GetDefaultModule(), alloc};
    arm64::JitTranslator translator{context};
    context.TickIR(address.Def());
    const auto begin = context.CurrentBufferSize();
    translator.EmitGetOperand(address.Def());
    context.EndInstructionScratch();
    const auto bytes = context.CurrentBufferSize() - begin;

    const bool enabled = swift::runtime::GetSvmConfig().abs_const_mat;
    REQUIRE(bytes == (enabled ? 8 : 12));
}

// --- x86 mul CF/OF must reach the flags register -----------------------------
//
// JitTranslator::SaveCV/SaveOF used to write their C/V bits into the HOST NZCV
// register (Msr) and then clear nzcv_dirty, which is exactly the state that
// makes MergeNZCV() do nothing: the bits were produced and then dropped. The
// x86 frontend cannot reach either function today -- MulWithFlags materialises
// CF/OF through a separate `SaveFlags(t + t, C|V)` producer rather than hanging
// the pseudo on the Mul, and the only frontend ir::Div is the flagless RCL/RCR
// modulus -- so no guest program and no exit code can catch this. The IR the
// backend accepts is wider than the IR the frontend currently emits, and this
// builds the missing shape directly.
//
// The check is on emitted code rather than on a running guest because
// executing a bare Block needs the trampoline/State machinery a unit test does
// not have. That is still a real check: whether the two C/V bits are OR-ed into
// the flags register (x26) or parked in host NZCV is precisely the difference
// between the broken and the fixed lowering.
static swift::runtime::ir::Block* BuildMulCarryOverflowBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7000});
    auto a = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{0, ValueType::U32});
    auto b = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{4, ValueType::U32});
    // 32x32 unsigned multiply: EmitMul widens with Umull and asks SaveCV
    // whether the upper half is nonzero, i.e. the x86 `mul` CF=OF rule.
    auto product = block->Mul(a, Operand{b});
    block->SaveFlags(product, Flags::Carry | Flags::Overflow);
    block->StoreUniform(Uniform{8, ValueType::U32}, product);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("SaveCV commits x86 CF/OF into the flags register") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildMulCarryOverflowBlock()};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);

    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    INFO(text);

    // HostFlagsBit::C = 29, HostFlagsBit::V = 28 -> (3u << 28) = 0x30000000,
    // and `flags` is x26 (backend/arm64/defines.h).
    REQUIRE(text.find("orr x26, x26, #0x30000000") != std::string::npos);
    // ...and it must NOT be stashed in host NZCV, where nothing collects it.
    // Nothing else in this block has any reason to write NZCV: the only guest
    // flag producer here is the multiply.
    REQUIRE(text.find("msr nzcv") == std::string::npos);
    REQUIRE(text.find("msr NZCV") == std::string::npos);
}

// --- CondSet is an arithmetic 0/1 value, not merely truthy ------------------
//
// Guest consumers historically fed CondSet only into truthiness operations,
// so replacing CSET (0/1) with CSETM (0/-1) survived the entire guest corpus.
// Build the missing IR shape directly: materialize EQ and add it to an integer
// before storing it. The disassembly check is the direct backend contract,
// following the SaveCV structural test above; a CSETM mutation fails here even
// though branches would continue to behave the same way.
static swift::runtime::ir::Block* BuildCondSetArithmeticBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7100});
    auto flags_value = block->LoadImm(Imm{std::uint64_t(0ull)}).SetType(ValueType::U64);
    block->SaveFlags(flags_value, Flags::NZ);
    auto one = block->CondSet(Cond::EQ).SetType(ValueType::U64);
    auto base = block->LoadImm(Imm{std::uint64_t(41ull)}).SetType(ValueType::U64);
    auto sum = block->Add(base, Operand{one});
    block->StoreUniform(Uniform{0, ValueType::U64}, sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("CondSet materializes exactly one for arithmetic consumers") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildCondSetArithmeticBlock()};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(block.get(), &reg_alloc, false, FeatureSet{});

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);

    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    INFO(text);

    REQUIRE(text.find("cset ") != std::string::npos);
    REQUIRE(text.find("csetm ") == std::string::npos);
    REQUIRE(text.find("add ") != std::string::npos);
}
