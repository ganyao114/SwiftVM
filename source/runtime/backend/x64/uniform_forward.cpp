#include "uniform_forward.h"

#include <algorithm>
#include <vector>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
struct ByteFact {
    ir::Value value;
    u8 byte{};
};
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
u32 Width(T type) {
    return type >= T::U8 && type <= T::V128 ? ir::GetValueSizeByte(type) : 0;
}
bool Range(u32 offset, u32 size, u32 extent) {
    return size && offset <= extent && size <= extent - offset;
}
bool Barrier(O op) {
    switch (op) {
        // Guest addresses can alias an escaped State/uniform pointer. These
        // instructions can also fault or enter runtime/SMC helpers. Keeping
        // stores in place preserves precise fault state; clearing facts
        // prevents reuse after an observer has changed the buffer.
        case O::LoadMemory: case O::StoreMemory:
        case O::LoadMemoryTSO: case O::StoreMemoryTSO:
        case O::MemoryCopy: case O::MemoryCopyTSO: case O::MemoryBarrierTSO:
        case O::CompareAndSwap: case O::CompareAndSwap128:
        case O::AtomicExchange: case O::AtomicFetchAdd: case O::AtomicRMW:
        case O::CheckMemoryAlignment:
        // No effect summaries are trusted here. A callback may read or write
        // any State field, even without an explicit uniform-pointer argument.
        case O::CallLambda: case O::CallLocation: case O::CallDynamic:
        case O::X87Op: case O::GetUniformAddress: case O::Cpuid:
        // Host mappings and explicit barriers expose state outside this pass's
        // byte table. Location operations are conservative runtime boundaries.
        case O::UniformBarrier: case O::XchgBarrier:
        case O::GetHostGPR: case O::SetHostGPR:
        case O::GetHostFPR: case O::SetHostFPR:
        case O::SetLocation: case O::GetLocation:
        // No path-sensitive facts, Phi insertion, or cross-block residency.
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
            return true;
        // Explicitly audited operations touch only SSA containers or the
        // separate flags word. Unknown/new opcodes stay conservative until
        // their State/callback effects are reviewed. In particular Cpuid's
        // caller-supplied helper address is opaque, unlike fixed math helpers.
        case O::LoadUniform: case O::StoreUniform: case O::LoadImm:
        case O::Nop: case O::AdvancePC: case O::Zero:
        case O::BitCast: case O::GetOperand: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb:
        case O::Mul: case O::MulSub: case O::MulHigh:
        case O::Div: case O::SignedDiv64: case O::Div128: case O::Div128Remainder:
        case O::CpuidUpper:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot:
        case O::GetFlags: case O::SaveFlags: case O::BranchOnlyFlags:
        case O::TestFlags: case O::TestNotFlags: case O::ClearFlags:
        case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
        case O::ByteSwap: case O::CountLeadingZeros64: case O::CountLeadingZeros32:
        case O::CountTrailingZeros64: case O::CountTrailingZeros32:
        case O::BitExtract: case O::BitInsert: case O::BitClear:
        case O::TestBit: case O::TestZero: case O::TestNotZero:
        case O::VecXor: case O::VecAnd: case O::VecOr: case O::VecAndNot:
        case O::CondSet:
            return false;
        default: return true;
    }
}
} // namespace

UniformForwardStats ForwardUniformLoads(ir::Block* block, u32 uniform_size) {
    UniformForwardStats stats;
    if (!block || !uniform_size) return stats;
    std::vector<ByteFact> facts(uniform_size);
    const auto clear = [&] { std::fill(facts.begin(), facts.end(), ByteFact{}); };
    const auto remember = [&](u32 offset, u32 size, ir::Value value) {
        for (u32 byte = 0; byte < size; ++byte) facts[offset + byte] = {value, u8(byte)};
    };
    for (auto& inst : block->GetInstList()) {
        if (Barrier(inst.GetOp())) { clear(); ++stats.barriers; continue; }
        if (inst.GetOp() == O::StoreUniform) {
            ++stats.stores_retained;
            const auto uniform = inst.GetArg<ir::Uniform>(0);
            const auto value = inst.GetArg<ir::Value>(1);
            const auto size = Width(value.Type());
            if (!Range(uniform.GetOffset(), size, uniform_size) || !value.Defined()) {
                clear(); continue;
            }
            // Store width belongs to its input view, not its descriptor. Keep
            // even unsupported producer facts so overlapping bytes invalidate
            // a previously valid complete source.
            remember(uniform.GetOffset(), size, value);
        } else if (inst.GetOp() == O::LoadUniform) {
            ++stats.loads_seen;
            const auto uniform = inst.GetArg<ir::Uniform>(0);
            const auto type = inst.ReturnType();
            const auto size = Width(uniform.GetType());
            if (type != uniform.GetType() || !Range(uniform.GetOffset(), size, uniform_size)) continue;
            // A narrow vector load initializes unused bytes. Raw aliases and
            // narrow vector producers may retain high poison, so never fold it.
            if (!Scalar(type) && type != T::V128) continue;
            const auto source = facts[uniform.GetOffset()];
            bool complete = source.value.Defined();
            for (u32 byte = 0; complete && byte < size; ++byte) {
                const auto& item = facts[uniform.GetOffset() + byte];
                complete = item.value == source.value && item.byte == source.byte + byte;
            }
            if (complete) {
                const auto actual = source.value.Def()->ReturnType();
                const auto source_size = Width(source.value.Type());
                if (Scalar(type) && Scalar(source.value.Type()) && Scalar(actual) &&
                    source.byte <= source_size && size <= source_size - source.byte) {
                    // An explicit extract also canonicalizes an apparently
                    // same-width view of a wider raw BitCast/GetResult alias.
                    inst.Reset();
                    inst.BitExtract(source.value, ir::Imm{u32(source.byte) * 8}, ir::Imm{size * 8}).SetReturn(type);
                    ++stats.scalar_loads;
                    ++stats.loads_forwarded;
                } else if (type == T::V128 && source.value.Type() == T::V128 && actual == T::V128 && source.byte == 0) {
                    inst.Reset(); inst.BitCast(source.value).SetReturn(T::V128);
                    ++stats.vector_loads;
                    ++stats.loads_forwarded;
                }
            }
            // This load (possibly now a canonical extraction) defines its own
            // complete buffer extent and dominates later straight-line reads.
            remember(uniform.GetOffset(), size, ir::Value{&inst});
        }
        // AdvancePC is metadata only: it changes no uniform byte, performs no
        // callback, and neither faults nor polls. Actual poll/exit boundaries
        // are branches/labels/terminals; stores remain committed before them.
    }
    return stats;
}
} // namespace swift::runtime::backend::x64
