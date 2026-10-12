#include <array>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include "aarch64/constants-aarch64.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/single_instruction_admission.h"

using namespace swift;
using namespace swift::runtime;
using namespace vixl::aarch64;
namespace {
constexpr VAddr kPc = 0x1000;
namespace admission = swift::arm64::detail;
using Query = bool (*)(u64, u32) noexcept;

struct Fetch final : MemoryInterface {
    u32 word;
    explicit Fetch(u32 word) : word(word) {}
    CodeFetchResult ReadCode(void* output, size_t address, size_t size) override {
        if (address != kPc || size != sizeof(word)) return CodeFetchResult::Fault;
        std::memcpy(output, &word, sizeof(word));
        return CodeFetchResult::Success;
    }
    bool Read(void*, size_t, size_t) override { FAIL("admission must not read guest data"); return false; }
    bool Write(void*, size_t, size_t) override { FAIL("admission must not write guest data"); return false; }
    void* GetPointer(void*) override { FAIL("DecodeSingle must use ReadCode"); return nullptr; }
};

struct Program {
    ir::Block block{ir::Location{kPc}};
    explicit Program(u32 word = FADD_d) {
        Fetch fetch{word};
        ir::Assembler assembler{&block};
        swift::arm64::A64Decoder decoder{kPc, &fetch, &assembler};
        REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Success);
    }
    ir::Inst& Call() {
        for (auto& inst : block.GetInstList()) if (inst.GetOp() == ir::OpCode::CallLambda) return inst;
        FAIL("expected an ARM FP helper call");
        std::abort();
    }
    ir::Inst& Source(int slot) { return *Call().ArgAt(slot).Get<ir::Value>().Def(); }
    bool Admitted() const { return swift::arm64::Arm64SingleInstructionAdmitted(block); }
};

ir::HelperCallTraits DecoderTraits() {
    return {.host_fp = ir::HostFpEffect::FPCRTransparent};
}

u64 UnknownHelper(u64, u64, u64) { FAIL("unknown helper must never execute"); return 0; }
void ReplaceTraits(Program& program, ir::HelperCallTraits traits) {
    const auto target = program.Call().ArgAt(0).Get<ir::Lambda>().GetImm();
    program.Call().SetArg(0, ir::Lambda{ir::DataClass{target}, traits});
}
}

TEST_CASE("ARM64 single instruction admission covers every audited FP helper target") {
    struct Case { const char* name; u32 word; Query query; };
    const Case cases[] = {
        {"scalar", FADD_d, admission::AdmitScalarFP},
        {"scalar pairwise", NEON_FADDP_scalar, admission::AdmitScalarPairwiseFP},
        {"half unary", NEON_FSQRT_H, admission::AdmitVectorFP16Unary},
        {"half binary", NEON_FADD_H, admission::AdmitFP16Binary},
        {"vector binary", NEON_FADD, admission::AdmitVectorFPBinary},
        {"vector unary", NEON_FSQRT, admission::AdmitVectorUnary},
        {"compare half zero", NEON_FCMEQ_H_zero, admission::AdmitFPCompare},
        {"compare half registers", NEON_FCMEQ_H, admission::AdmitFPCompare},
        {"compare zero", NEON_FCMEQ_zero, admission::AdmitFPCompare},
        {"compare registers", NEON_FCMEQ, admission::AdmitFPCompare},
        {"integer half", NEON_SCVTF_H, admission::AdmitFPInteger},
        {"integer", NEON_SCVTF, admission::AdmitFPInteger},
        {"fixed", NEON_SCVTF_imm | (63u << 16), admission::AdmitFixedSIMD},
        {"range", FRINT32Z_s, admission::AdmitRangeRound},
        {"precision", NEON_FCVTL, admission::AdmitPrecision},
        {"estimate half", NEON_FRECPE_H, admission::AdmitEstimate},
        {"estimate", NEON_FRECPE, admission::AdmitEstimate},
        {"reduction half across", NEON_FMAXV_H | NEON_Q, admission::AdmitReduction},
        {"reduction half pairwise", NEON_FADDP_H, admission::AdmitReduction},
        {"reduction across", NEON_FMAXV | NEON_Q, admission::AdmitReduction},
        {"reduction pairwise", NEON_FADDP, admission::AdmitReduction},
        {"scalar half misc", NEON_FCMEQ_H_zero_scalar, admission::AdmitScalarFP16Misc},
        {"fused half", NEON_FMLA_H, admission::AdmitVectorFMA},
        {"fused", NEON_FMLA, admission::AdmitVectorFMA},
        {"extended half", NEON_FMULX_H, admission::AdmitExtendedFP},
        {"extended", NEON_FMULX, admission::AdmitExtendedFP},
        {"indexed", NEON_FMUL_H_byelement, admission::AdmitIndexedFP},
    };
    std::set<u64> targets;
    for (const auto& entry : cases) {
        INFO(entry.name);
        Program program{entry.word};
        const auto target = program.Call().ArgAt(0).Get<ir::Lambda>().GetImm().Get();
        REQUIRE(entry.query(target, entry.word));
        REQUIRE(program.Admitted());
        targets.insert(target);
    }
    REQUIRE(targets.size() == 25);
}

TEST_CASE("ARM64 single instruction admission allows native operations branches and defined interrupts") {
    for (u32 word : {0x91000420u, 0xF9400020u, 0xF9000020u, 0x94000001u,
                     0xD63F0020u, 0xD4000001u, 0xD4200000u, 0xD4400000u, 0x00000000u}) {
        INFO(word);
        Program program{word};
        REQUIRE(program.Admitted());
    }
    Program exclusive{0xC85F7C20}; // LDXR X0,[X1]: decoder static fallback.
    REQUIRE_FALSE(exclusive.Admitted());
}

TEST_CASE("ARM64 single instruction admission rejects unknown null and indirect helper targets") {
    SECTION("unknown fixed target") {
        Program program;
        program.Call().SetArg(0, ir::Lambda{ir::DataClass{ir::Imm{reinterpret_cast<u64>(FptrCast(&UnknownHelper))}}, DecoderTraits()});
        REQUIRE_FALSE(program.Admitted());
    }
    SECTION("void target") {
        Program program;
        program.Call().SetArg(0, ir::Lambda{});
        REQUIRE_FALSE(program.Admitted());
    }
    SECTION("SSA target") {
        Program program;
        program.Call().SetArg(0, ir::Lambda{program.Call().ArgAt(2).Get<ir::Value>()});
        REQUIRE_FALSE(program.Admitted());
    }
    for (auto opcode : {ir::OpCode::CallDynamic, ir::OpCode::CallLocation, ir::OpCode::Cpuid}) {
        ir::Block block{ir::Location{kPc}};
        block.AppendInst(opcode);
        REQUIRE_FALSE(swift::arm64::Arm64SingleInstructionAdmitted(block));
    }
}

TEST_CASE("ARM64 single instruction admission requires exact helper ABI and effect metadata") {
    const std::array<std::function<void(ir::HelperCallTraits&)>, 8> changes{
        [](auto& traits) { traits.uniform = ir::UniformEffectId::None; },
        [](auto& traits) { traits.abi = ir::HelperABI::PreserveAllLeaf; },
        [](auto& traits) { traits.host_fp = ir::HostFpEffect::MayTouch; },
        [](auto& traits) { traits.host_registers = ir::HostRegisterEffect::GeneralOnly; },
        [](auto& traits) { traits.guest_state = ir::HelperGuestStateEffect::None; },
        [](auto& traits) { traits.fault = ir::HelperFaultEffect::NoDirectFault; },
        [](auto& traits) { traits.reentry = ir::HelperReentryEffect::NoReentry; },
        [](auto& traits) { traits.host_flags = ir::HostFlagsEffect::PreservesNZCV; },
    };
    for (const auto& change : changes) {
        Program program;
        auto traits = DecoderTraits();
        change(traits);
        ReplaceTraits(program, traits);
        REQUIRE_FALSE(program.Admitted());
    }
}

TEST_CASE("ARM64 single instruction admission checks helper arguments without following foreign SSA") {
    SECTION("context offset") { Program program; program.Source(1).SetArg(0, ir::Imm{u32{8}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("context pointer immediate") {
        Program program;
        program.Source(1).SetInst(ir::OpCode::LoadImm, ir::Imm{u64{0x12345678}});
        REQUIRE_FALSE(program.Admitted());
    }
    SECTION("wrong context immediate width") { Program program; program.Source(1).SetArg(0, ir::Imm{u64{0}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("invalid word") { Program program; program.Source(2).SetArg(0, ir::Imm{u64{0}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("word truncation") { Program program; program.Source(2).SetArg(0, ir::Imm{u64{FADD_d} | (u64{1} << 32)}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("word immediate width") { Program program; program.Source(2).SetArg(0, ir::Imm{u32{FADD_d}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("wrong PC") { Program program; program.Source(3).SetArg(0, ir::Imm{u64{kPc + 4}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("missing argument") { Program program; program.Call().SetArg(3, ir::Void{nullptr}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("extra physical argument") { Program program; program.Call().SetArg(4, ir::Imm{u64{0}}); REQUIRE_FALSE(program.Admitted()); }
    SECTION("wrong return width") { Program program; program.Call().SetReturn(ir::ValueType::U32); REQUIRE_FALSE(program.Admitted()); }
    SECTION("cast argument width") {
        Program program;
        program.Call().SetArg(2, program.Call().ArgAt(2).Get<ir::Value>().SetCastType(ir::ValueType::U32));
        REQUIRE_FALSE(program.Admitted());
    }
    SECTION("foreign source") {
        ir::Block foreign{ir::Location{0x2000}};
        const auto value = foreign.LoadImm<ir::U64>(ir::Imm{u64{kPc}}).SetType(ir::ValueType::U64);
        Program program;
        program.Call().SetArg(3, value);
        REQUIRE_FALSE(program.Admitted());
    }
    SECTION("forward source") {
        Program program;
        const auto value = program.block.LoadImm<ir::U64>(ir::Imm{u64{kPc}}).SetType(ir::ValueType::U64);
        program.Call().SetArg(3, value);
        REQUIRE_FALSE(program.Admitted());
    }
}

TEST_CASE("ARM64 single instruction admission rejects helper words that bypass decoder safety guards") {
    struct Case { u32 valid, invalid; };
    const Case cases[] = {
        {NEON_SCVTF_imm | (63u << 16), NEON_SCVTF_imm}, // immh=0: invalid lane divisor.
        {NEON_FADDP | NEON_Q | (1u << 22), NEON_FADDP | (1u << 22)}, // Pairwise 1D.
        {NEON_FCVTXN | (1u << 22), NEON_FCVTXN}, // FCVTXN half destination.
        {NEON_FMUL_H_byelement | NEON_Q | (3u << 22),
         NEON_FMUL_H_byelement | NEON_Q | (3u << 22) | (1u << 21)}, // Indexed D L1.
    };
    for (const auto& entry : cases) {
        INFO(entry.valid);
        Program program{entry.valid};
        REQUIRE(program.Admitted());
        program.Source(2).SetArg(0, ir::Imm{u64{entry.invalid}});
        REQUIRE_FALSE(program.Admitted());
    }
}

