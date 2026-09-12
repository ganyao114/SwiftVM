#include "../support/case_support.h"

TEST_CASE("Vector emission grows a full code buffer before writing") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;

    for (unsigned operation = 0; operation < 3; ++operation) {
        CAPTURE(operation);
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = kArm64,
        };
        AddressSpace address_space{config};
        auto module = address_space.GetDefaultModule();
        IntrusivePtr<Block> block{new Block(0, Location{0x2f00})};
        auto left = block->GetHostFPR(HostRegIndex(17), Imm{0u})
                            .SetType(ValueType::V128);
        auto right = block->GetHostFPR(HostRegIndex(18), Imm{0u})
                             .SetType(ValueType::V128);
        auto result = operation == 0
                ? block->VecMovMask(left, Imm{8u}).SetType(ValueType::U32)
                : block->VecPclMul(left, right, Imm{operation == 1 ? 0u : 0x11u})
                          .SetType(ValueType::V128);
        block->StoreUniform(Uniform{0, result.Type()}, result);
        block->SetTerminal(terminal::ReturnToDispatch{});
        block->ReIdInstr();
        const auto features = ResolveFeatureSet(module->GetModuleConfig());
        RegAlloc allocation{block->MaxInstrId(),
                            address_space.GetTrampolines().GetGPRRegs(),
                            address_space.GetTrampolines().GetFPRRegs(), features, true};
        RegisterAllocPass::Run(block.get(), &allocation, true, features);
        allocation.MapRegister(left.Id(), HostFPR{17});
        allocation.MapRegister(right.Id(), HostFPR{18});

        constexpr size_t capacity = 64;
        vixl::aarch64::MacroAssembler assembler{capacity};
        arm64::JitContext context{module, allocation, assembler};
        arm64::JitTranslator translator{context};
        // Fixed input reads emit no copies, so the vector operation is the
        // first writer after this boundary. It must reserve its own space.
        for (size_t offset = 0; offset < capacity;
             offset += vixl::aarch64::kInstructionSize) {
            assembler.Nop();
        }
        REQUIRE(assembler.GetBuffer()->GetRemainingBytes() == 0);
        translator.Translate(block.get());
        REQUIRE(assembler.GetBuffer()->GetSizeInBytes() <=
                assembler.GetBuffer()->GetCapacity());
        REQUIRE(assembler.GetBuffer()->GetCapacity() > capacity);
        context.Finish();
    }
}
