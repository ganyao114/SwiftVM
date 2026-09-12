#include "../support/case_support.h"

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

std::string MaskDisasmAddresses(std::string text) {
    constexpr std::string_view kAddressPrefix{"(addr 0x"};
    constexpr std::string_view kReplacement{"(addr <adjusted>)"};
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
                             bool afp_nan,
                             bool afp_minmax = false) {
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
    ModuleConfig module_config{};
    module_config.feature_overrides.Set(FeatureId::sse_afp_minmax,
                                        afp_minmax);
    auto module = address_space.MapModule(0x2d00,
                                          0x2d01,
                                          module_config);
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

    const auto features = ResolveFeatureSet(module_config);
    RegAlloc reg_alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(),
                       features};
    RegisterAllocPass::Run(block.get(), &reg_alloc, scalar_insert, features);
    arm64::JitContext context{module, reg_alloc};
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

const char* AFPShapeName(AFPShapeOp op);

TEST_CASE("AFP scalar MIN MAX preserves left lanes with any destination") {
    for (const auto op : {AFPShapeOp::Min, AFPShapeOp::Max}) {
        for (const swift::u32 lane_bits : {swift::u32{32}, swift::u32{64}}) {
            INFO(AFPShapeName(op) << " f" << lane_bits);

            const auto off = CompileAFPShape(
                    op, lane_bits, true, true, true, false);
            const auto on = CompileAFPShape(
                    op, lane_bits, true, true, true, true);
            INFO("OFF:\n" << off.text << "ON:\n" << on.text);
            REQUIRE(on.instructions + 2 == off.instructions);
            REQUIRE(on.text.find(op == AFPShapeOp::Max ? "fmax" : "fmin") !=
                    std::string::npos);
            REQUIRE(on.text.find("fcmp") == std::string::npos);
            REQUIRE(on.text.find("ins") == std::string::npos);

            const auto untied_off = CompileAFPShape(
                    op, lane_bits, true, false, true, false);
            const auto untied_on = CompileAFPShape(
                    op, lane_bits, true, false, true, true);
            INFO("untied OFF:\n" << untied_off.text
                                  << "untied ON:\n" << untied_on.text);
            REQUIRE(untied_on.instructions + 3 == untied_off.instructions);
            REQUIRE(untied_on.text.find(op == AFPShapeOp::Max ? "fmax" : "fmin") !=
                    std::string::npos);
            REQUIRE(untied_on.text.find("fcmp") == std::string::npos);
            REQUIRE(untied_on.text.find("bsl") == std::string::npos);

            const auto no_contract = CompileAFPShape(
                    op, lane_bits, true, true, false, true);
            const auto no_contract_off = CompileAFPShape(
                    op, lane_bits, true, true, false, false);
            REQUIRE(no_contract.instructions == no_contract_off.instructions);
            REQUIRE(MaskDisasmAddresses(no_contract.text) ==
                    MaskDisasmAddresses(no_contract_off.text));
        }
    }
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
    REQUIRE(MaskDisasmAddresses(
                    "b.mi #+0x8 (addr 0xaA09)\nmov x0, #0x123") ==
            "b.mi #+0x8 (addr <adjusted>)\nmov x0, #0x123");

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
            REQUIRE(MaskDisasmAddresses(on.text) ==
                    MaskDisasmAddresses(off.text));
        }
    }
}
