#include "test_support.h"
#include "runtime/backend/loongarch64/single_instruction_admission.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/single_instruction_admission.h"
#include "runtime/ir/atomic_rmw.h"

#include <array>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
struct Unit {
    ir::Block block{ir::Location{0xc000}};
    Unit() { block.SetTerminal(ir::terminal::ReturnToDispatch{}); }
    template<class... Args> ir::Inst* Add(O op, T type, const Args&... args) {
        auto* inst = ir::Inst::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); return inst;
    }
    ir::Value Constant(u64 value = 1, T type = T::U64) { return ir::Value{Add(O::LoadImm, type, ir::Imm{value})}; }
    bool Admit(size_t bytes = 4096) { block.ReIdInstr(); return la::SingleInstructionAdmitted(block, bytes); }
};
class Word final : public MemoryInterface {
public:
    explicit Word(u32 word) : word(word) {}
    CodeFetchResult ReadCode(void* out, size_t address, size_t size) override {
        ++fetches;
        if (address != 0xc000 || size != 4) return CodeFetchResult::Fault;
        std::memcpy(out, &word, 4); return CodeFetchResult::Success;
    }
    bool Read(void*, size_t, size_t) override { ++unsafe; return false; }
    bool Write(void*, size_t, size_t) override { ++unsafe; return false; }
    void* GetPointer(void*) override { ++unsafe; return nullptr; }
    u32 word{}, fetches{}, unsafe{};
};
}

void SingleInstructionAdmissions() {
    const auto before = checks;
    for (u32 word : {0xd503201fu, 0xd28000e0u, 0x91000400u, 0xb1000400u,
                    0xba010000u, 0xfa010000u, 0xf9400020u, 0xb9400020u,
                    0xf9000020u, 0xb9000020u, 0x14000004u, 0xd61f0000u,
                    0xd63f03c0u, 0xd65f03c0u, 0xd4000001u, 0x1e612800u}) {
        Word memory{word}; ir::Block block{ir::Location{0xc000}}; ir::Assembler as{&block};
        swift::arm64::A64Decoder decode{0xc000, &memory, &as};
        Check(decode.DecodeSingle() == CodeFetchResult::Success && block.HasTerminal(),
              "ARM64 private capability fixture decodes exactly one word");
        block.ReIdInstr();
        Check(swift::arm64::Arm64SingleInstructionAdmitted(block) &&
              la::SingleInstructionAdmitted(block, sizeof(swift::arm64::ThreadContext64)),
              "ARM64 arithmetic, memory, branches, traps and audited FP helpers have positive capabilities");
        Check(memory.fetches == 1 && memory.unsafe == 0, "admission uses bounded fresh decode without unsafe fetch");
    }
    for (T type : {T::U8, T::U16, T::U32, T::U64, T::V64, T::V128, T::V256})
    for (bool immediate : {false, true}) {
        Unit u;
        auto base = u.Constant(24);
        const auto address = immediate ? ir::Operand{ir::Imm{u64{24}}} : ir::Operand{base};
        auto* loaded = u.Add(O::LoadMemory, type, address);
        u.Add(O::StoreMemoryTSO, T::VOID, address, ir::Value{loaded});
        Check(u.Admit() == (type != T::V256), "private memory inventory admits exact supported widths only");
    }
    for (u64 operation = 0; operation <= u64(ir::AtomicRMWOp::UnsignedMin) + 1; ++operation)
    for (T type : {T::U8, T::U16, T::U32, T::U64}) {
        Unit u; auto address = u.Constant(24), operand = u.Constant(1, type), carry = u.Constant();
        u.Add(O::AtomicRMW, type, ir::Imm{operation}, address, operand, carry);
        Check(u.Admit() == (operation <= u64(ir::AtomicRMWOp::UnsignedMin)),
              "audited atomics retain width, explicit carry and supported RMW operation");
    }
    {
        Unit u; auto value = u.Constant();
        u.Add(O::CompareAndSwap128, T::V128, value, value, value, value, value);
        Check(u.Admit(), "five scalar halves are admitted for the audited 128-bit CAS kernel");
    }
    for (u64 size : {u64{0}, u64{1}, u64{16}, u64{32}, u64{33}, UINT64_MAX}) {
        Unit u; u.Add(O::MemoryCopy, T::VOID, ir::Lambda{ir::Imm{u64{16}}},
                      ir::Lambda{ir::Imm{u64{32}}}, ir::Imm{size});
        Check(u.Admit() == (size <= 32), "first private bulk capability has an explicit fixed bound");
    }
    {
        Unit u; auto value = u.Constant();
        u.Add(O::StoreUniform, T::VOID, ir::Uniform{4090, T::U64}, value);
        Check(!u.Admit(), "uniform boundary is validated before a preceding effect can commit");
    }
    {
        Unit u; auto* value = u.Add(O::LoadImm, T::U64, ir::Imm{u64{1}});
        Check(u.Admit(), "dense private SSA fixture starts valid");
        value->SetId(17);
        Check(!la::SingleInstructionAdmitted(u.block, 4096), "sparse private homes fail closed");
    }
    {
        Unit u; u.Add(O::AdvancePC, T::VOID, ir::Imm{u32{4}});
        Check(!u.Admit(), "multi-instruction AdvancePC is refused");
    }
    {
        Unit foreign, u; auto input = foreign.Constant();
        u.Add(O::BitCast, T::U64, input);
        Check(!u.Admit(), "foreign producer cannot escape retained private IR");
    }
    for (bool dominates : {false, true}) {
        Unit u; auto cond = u.Constant(1, T::U8);
        ir::Value value;
        if (dominates) value = u.Constant(42);
        auto* branch = u.Add(O::Goto, T::VOID, cond);
        if (!dominates) value = u.Constant(42);
        u.Add(O::BindLabel, T::VOID, ir::Value{branch});
        u.Add(O::StoreUniform, T::VOID, ir::Uniform{16, T::U64}, value);
        Check(u.Admit() == dominates, "both local paths require dominating data definitions");
    }
    {
        Unit u; auto cond = u.Constant(1, T::U8); auto* branch = u.Add(O::Goto, T::VOID, cond);
        auto* label = u.Add(O::BindLabel, T::VOID, ir::Value{branch});
        u.block.RemoveInst(label); u.block.InsertBefore(label, branch);
        Check(!u.Admit(), "IR backedges cannot become unpolled private loops");
    }
    for (O op : {O::GetHostGPR, O::GetHostFPR}) {
        Unit u; u.Add(op, T::U64, ir::Imm{u32{0}}, ir::Imm{u32{0}});
        Check(!u.Admit(), "host register stubs are outside positive single capabilities");
    }
    {
        Unit u; auto value = u.Constant();
        u.Add(O::X87Op, T::U64, value, ir::Imm{u64{0}}, value);
        Check(!u.Admit(), "stateful x87 dispatcher cannot acquire private recovery rights");
    }
    {
        Unit u; auto value = u.Constant();
        u.Add(O::CallLambda, T::U64, ir::Lambda{ir::Imm{u64{1}}}, value, value, value);
        Check(u.Admit() && !swift::arm64::Arm64SingleInstructionAdmitted(u.block),
              "backend shape does not substitute for frontend helper identity admission");
    }
    std::cout << "PASS private single-instruction admission " << checks - before << " checks\n";
}

} // namespace swift::tests::loongarch64
