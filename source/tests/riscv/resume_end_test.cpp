#include <cstdio>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/restricted_instruction_resume.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/include/sruntime.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

namespace {
unsigned checks{};

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

IntrusivePtr<Block> Program(bool wide, u64 end) {
    IntrusivePtr<Block> block{new Block(Location{0x1000})};
    Assembler as{block.get()};
    const auto type = wide ? ValueType::V256 : ValueType::U64;
    auto live = as.LoadUniform(Uniform{0, type}).SetType(type);
    auto* first = as.LoadUniform(Uniform{64, ValueType::U64}).SetType(ValueType::U64).Def();
    as.StoreUniform(Uniform{32, type}, live);
    auto branch = as.Goto(BOOL{Value{first}});
    as.BindLabel(branch);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label);
    block->InsertBefore(label, first);
    as.AdvancePC(Imm{4u});
    as.SetLocation(Lambda{Imm{u64{0x1004}}});
    block->SetTerminal(terminal::ReturnToHost{});
    if (end) block->SetEndLocation(Location{end});
    block->ReIdInstr();
    return block;
}

void Run(bool read_only, bool wide, u64 end) {
    struct GuestMemory {
        void* base;
        size_t size;
        ~GuestMemory() { if (base != MAP_FAILED) munmap(base, size); }
    } memory{MAP_FAILED, static_cast<size_t>(sysconf(_SC_PAGESIZE)) * 2};
    memory.base = mmap(nullptr, memory.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(memory.base != MAP_FAILED, "real guest pages back SMC registration");
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.backend_isa = kRiscv64;
    config.enable_jit = true;
    config.uniform_buffer_size = 128;
    config.stack_alignment = 16;
    config.static_program = read_only;
    config.memory_base = memory.base;
    config.guest_addr_mask = memory.size - 1;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    auto block = Program(wide, end);
    Check(module->Push(block.get()), "module owns canonical IR");
    auto* code = static_cast<u8*>(TranslateIR(module, block));
    Check(code != nullptr, "resume block compiles");
    auto pin = module->PinInstructionCode(module, code);
    Check(bool(pin), "fresh instruction pin exists after publication");
    Check(module->ValidateInstructionPin(*pin), "fresh instruction pin validates after publication");
    const auto& descriptor = *pin->descriptor;
    Check(block->GetEndLocation() == Location{0x1004}, "AdvancePC normalizes canonical end");
    Check(descriptor.completion_pc == 0x1004, "descriptor captures normalized completion PC");
    Check(descriptor.emitted->GetEndLocation() == Location{0x1004}, "wide clone retains normalized end");
    Check((descriptor.emitted != descriptor.original) == wide, "only wide shape clones IR");
    Check(descriptor.original_identity == InstructionIRIdentity(*descriptor.original), "canonical identity remains current");
    Check(descriptor.ir_identity == InstructionIRIdentity(*descriptor.emitted), "emitted identity remains current");
    block->SetEndLocation(Location{0x1008});
    Check(!module->ValidateInstructionPin(*pin), "later IR mutation still invalidates pins");
    pin.reset();
    Check(module->InspectInstructionPin(code).pins == 0, "fresh pin releases its owner");
}
}

int main() {
    try {
        for (bool read_only : {false, true})
            for (bool wide : {false, true})
                for (u64 end : {u64{0}, u64{0x1002}, u64{0x1004}})
                    Run(read_only, wide, end);
        std::printf("PASS resume-end cases=12 checks=%u\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL resume-end checks=%u: %s\n", checks, error.what());
        return 1;
    }
}
