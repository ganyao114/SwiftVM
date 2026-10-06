#include <array>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "translator/x86/translator.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

IntrusivePtr<Block> BitOperations(u32 width, bool keep_inputs) {
    IntrusivePtr<Block> block{new Block(0, Location{0xb200})};
    std::memset(&block->GetJitCache(), 0, sizeof(JitCache));
    const auto data = block->LoadUniform(Uniform{0, ValueType::U64});
    const auto acc = block->LoadUniform(Uniform{8, ValueType::U64});
    const auto zero = block->LoadImm(Imm{u64{0}});
    const auto negative = block->Sub(zero, Operand{Imm{u64{1}}});
    block->SaveFlags(negative, Flags::NZCV);
    const auto count = block->PopCount(data);
    block->StoreUniform(Uniform{16, ValueType::U64}, count);
    const auto crc = block->Crc32c(acc, data, Imm{width});
    block->StoreUniform(Uniform{24, ValueType::U32}, crc);
    block->StoreUniform(Uniform{32, ValueType::U64},
                        block->TestFlags(Flags::Negate).SetType(ValueType::U64));
    if (keep_inputs) {
        block->StoreUniform(Uniform{48, ValueType::U64}, data);
        block->StoreUniform(Uniform{56, ValueType::U64}, acc);
    }
    block->SetTerminal(terminal::LinkBlock{Location{0xb201}});
    block->ReIdInstr();
    return block;
}

u32 ReferenceCrc(u32 crc, u64 data, u32 width) {
    for (u32 byte = 0; byte < width / 8; ++byte) {
        crc ^= u8(data);
        data >>= 8;
        for (u32 bit = 0; bit < 8; ++bit) {
            const bool low = crc & 1;
            crc >>= 1;
            if (low) crc ^= 0x82f63b78;
        }
    }
    return crc;
}

Config BitConfig(bool hardware) {
    return Config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 64,
            .static_program = true,
            .arm64_features = hardware ? Arm64Features::CRC32 : Arm64Features::None,
    };
}
}  // namespace

TEST_CASE("scalar bit operations emit without host helpers", "[scalar-bits][codegen]") {
    for (const bool hardware : {false, true}) {
        CAPTURE(hardware);
        auto config = BitConfig(hardware);
        AddressSpace address_space{config};
        auto block = BitOperations(64, true);
        FeatureSet features{};
        RegAlloc alloc{block->MaxInstrId(),
                       address_space.GetTrampolines().GetGPRRegs(),
                       address_space.GetTrampolines().GetFPRRegs(), features};
        RegisterAllocPass::Run(block.get(), &alloc, false, features);
        arm64::JitContext context{address_space.GetDefaultModule(), alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        auto& masm = context.GetMasm();
        auto* first = masm.GetBuffer()->GetStartAddress<const vixl::aarch64::Instruction*>();
        auto* last = masm.GetBuffer()->GetEndAddress<const vixl::aarch64::Instruction*>();
        vixl::aarch64::Decoder decoder;
        vixl::aarch64::Disassembler disassembler;
        decoder.AppendVisitor(&disassembler);
        std::string output;
        for (auto* instruction = first; instruction < last;
             instruction = instruction->GetNextInstruction()) {
            decoder.Decode(instruction);
            output += disassembler.GetOutput();
            output += '\n';
        }
        INFO(output);
        REQUIRE(output.find("cnt v") != std::string::npos);
        REQUIRE(output.find("addv b") != std::string::npos);
        REQUIRE(output.find("blr ") == std::string::npos);
        REQUIRE((output.find("crc32cx ") != std::string::npos) == hardware);
    }
}

TEST_CASE("scalar bit operations preserve inputs and pending NZCV", "[scalar-bits][production]") {
#if defined(__aarch64__)
    using Instance = swift::translator::x86::X86Instance;
    std::unique_ptr<Instance, decltype(&Instance::Destroy)> instance{Instance::Make(),
                                                                  Instance::Destroy};
    const bool has_crc = True(instance->GetAddressSpace()->GetConfig().arm64_features &
                              Arm64Features::CRC32);
    for (const bool hardware : {false, true}) {
        if (hardware && !has_crc) continue;
        for (const bool keep_inputs : {false, true}) {
            for (const u32 width : {8u, 16u, 32u, 64u}) {
                CAPTURE(hardware, keep_inputs, width);
                auto config = BitConfig(hardware);
                AddressSpace address_space{config};
                auto block = BitOperations(width, keep_inputs);
                const auto entry = TranslateIR(address_space.GetDefaultModule(), block);
                REQUIRE(entry != nullptr);
                address_space.PushCodeCache(0xb200, entry);
                for (const u64 data : {u64{0}, UINT64_MAX, u64{0x8000000000000000},
                                       u64{0x0123456789abcdef}}) {
                    for (const u64 acc : {u64{0}, UINT64_MAX, u64{0xfedcba9812345678}}) {
                        CAPTURE(data, acc);
                        Runtime runtime{&address_space};
                        std::array<u64, 8> state{data, acc};
                        std::memcpy(runtime.GetUniformBuffer().data(), state.data(), sizeof(state));
                        runtime.SetLocation(0xb200);
                        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
                        std::memcpy(state.data(), runtime.GetUniformBuffer().data(), sizeof(state));
                        REQUIRE(state[16 / 8] == std::popcount(data));
                        REQUIRE(u32(state[24 / 8]) == ReferenceCrc(u32(acc), data, width));
                        REQUIRE(state[32 / 8] == 1);
                        if (keep_inputs) {
                            REQUIRE(state[48 / 8] == data);
                            REQUIRE(state[56 / 8] == acc);
                        }
                    }
                }
            }
        }
    }
#else
    SUCCEED("Native scalar bit operations require an AArch64 host");
#endif
}
