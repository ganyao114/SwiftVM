#include "jit_context.h"

#include "runtime/backend/context.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/module.h"
#include "runtime/backend/address_space.h"
#include <stdexcept>
#include "runtime/backend/riscv64/defines.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/block.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;

void JitContext::ConfigureModule(Module* owner) {
    module = owner;
    direct_links = owner && owner->IsDirectLinkConfigured();
    rsb_enabled = owner && True(owner->GetAddressSpace().GetConfig().global_opts & Optimizations::ReturnStackBuffer);
}

u32 JitContext::DispatchIndex(u64 guest) {
    if (!module) throw std::runtime_error("RV64 dispatch slot requires a module");
    return module->GetDispatchIndex(ir::Location{guest});
}

void JitContext::EnsureSpace() {
    auto& buffer = masm.GetCodeBuffer();
    if (buffer.GetRemainingBytes() < 8192)
        buffer.Grow(buffer.GetSizeInBytes() + 16384);
}

void JitContext::HostAddress(GPR result, u64 address) {
    EnsureSpace();
    auto [it, inserted] = host_address_indices.emplace(address, host_addresses.size());
    if (inserted) host_addresses.push_back(address);
    host_references.push_back({CurrentBufferSize(), it->second, result});
    masm.AUIPC(result, 0); masm.LD(result, 0, result);
}

void JitContext::FinishHostAddresses() {
    if (host_addresses.empty()) return;
    auto& buffer = masm.GetCodeBuffer();
    buffer.Grow(buffer.GetSizeInBytes() + host_addresses.size() * sizeof(u64) + 4096);
    if (CurrentBufferSize() & 7) masm.NOP();
    buffer.Emit32(kRiscvHostPoolMagic);
    buffer.Emit32(host_addresses.size());
    const auto pool = CurrentBufferSize();
    for (const auto address : host_addresses) buffer.Emit(address);
    const auto end = buffer.GetCursorOffset();
    // RewindCursor only moves backward. Patch later references first.
    for (auto it = host_references.rbegin(); it != host_references.rend(); ++it) {
        const auto& ref = *it;
        const s64 delta = s64(pool) + ref.index * sizeof(u64) - ref.offset;
        if (delta < INT32_MIN || delta > INT32_MAX)
            throw std::runtime_error("RV64 host literal exceeds AUIPC reach");
        buffer.RewindCursor(ref.offset);
        masm.AUIPC(ref.reg, u32((delta + 0x800) >> 12) & 0xfffff);
        masm.LD(ref.reg, s32(((delta & 0xfff) ^ 0x800) - 0x800), ref.reg);
    }
    buffer.AdvanceCursor(end);
    host_references.clear(); host_addresses.clear(); host_address_indices.clear();
}

void JitContext::MarkABICall() {
    ReleaseMemoryLease();
    abi_calls = true;
    if (eager_abi_save) return;
    // The entry frame saves every register modified by this JIT block. The
    // remaining GP/FP registers keep their entry values until the first C ABI
    // call. A shared cold stub saves them once and selects complete recovery
    // in case a fault abandons the callee's restores. Only ra/t4/t5/t6 are
    // scratch here; ra already has an entry-frame home.
    ASSERT(abi_recovery && abi_save);
    Label saved;
    Load(t5, state, state_offset_riscv_recovery_pc);
    masm.LILabel(t4, abi_recovery);
    masm.BEQ(t5, t4, &saved);
    masm.LILabel(t5, abi_save);
    masm.JALR(ra, 0, t5);
    masm.Bind(&saved);
}

void JitContext::Address(GPR result, GPR base, s64 offset) {
    if (offset >= -2048 && offset <= 2047) {
        masm.ADDI(result, base, static_cast<s32>(offset));
    } else {
        // t6 is reserved for addresses, never an SSA or operand register.
        ASSERT(base != t6);
        masm.LI(t6, static_cast<u64>(offset));
        masm.ADD(result, base, t6);
    }
}

void JitContext::Load(GPR result, GPR base, s64 offset, u32 size) {
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.LBU(result, immediate, base); break;
        case 2: masm.LHU(result, immediate, base); break;
        case 4: masm.LWU(result, immediate, base); break;
        case 8: masm.LD(result, immediate, base); break;
        default: PANIC("unsupported RV64 scalar load size {}", size);
    }
}

void JitContext::Store(GPR value, GPR base, s64 offset, u32 size) {
    ASSERT(value != t6);
    if (offset < -2048 || offset > 2047) {
        Address(t6, base, offset);
        base = t6;
        offset = 0;
    }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.SB(value, immediate, base); break;
        case 2: masm.SH(value, immediate, base); break;
        case 4: masm.SW(value, immediate, base); break;
        case 8: masm.SD(value, immediate, base); break;
        default: PANIC("unsupported RV64 scalar store size {}", size);
    }
}

void JitContext::Read(GPR result, ir::Value value) { ReadPart(result, value, 0); }

void JitContext::ReadPart(GPR result, ir::Value value, u32 part) {
    const auto source = SourcePart(value, part, result);
    if (source != result) masm.MV(result, source);
}

GPR JitContext::SourceRegister(ir::Value value, GPR scratch) { return SourcePart(value, 0, scratch); }

bool JitContext::ZeroHigh(ir::Inst* inst) {
    if (inst->GetOp() == ir::OpCode::BitCast || inst->GetOp() == ir::OpCode::GetResult)
        return ZeroHigh(inst->GetArg<ir::Value>(0).Def());
    return !ir::IsFloatValueType(inst->ReturnType());
}

GPR JitContext::SourcePart(ir::Value value, u32 part, GPR scratch) {
    ASSERT(part < 2);
    for (const auto& binding : phi_bindings) if (binding.inst == value.Def()) {
        if (binding.gpr_indices[0] != UINT32_MAX)
            return binding.gpr_indices[part] == UINT32_MAX ? x0 : scalar_registers[binding.gpr_indices[part]];
        if (binding.vector_index != UINT32_MAX) {
            SetVectorType(64, 2);
            const auto source = Vec{binding.vector_index + 8};
            if (part) { masm.VSLIDEDOWN(v7, source, 1u); masm.VMV_XS(scratch, v7); }
            else masm.VMV_XS(scratch, source);
            return scratch;
        }
    }
    // Reads never allocate: all internal select arms share one mapping.
    for (size_t i = 0; i < cached_values.size(); ++i) {
        if (cached_values[i] == value.Def() && cached_parts[i] == part) {
            ++value_stats.cache_hits;
            return scalar_registers[i];
        }
    }
    for (size_t i = 0; i < cached_vectors.size(); ++i) {
        if (cached_vectors[i] == value.Def()) {
            SetVectorType(64, 2);
            const auto source = Vec{static_cast<u32>(i + 8)};
            if (part) { masm.VSLIDEDOWN(v7, source, 1u); masm.VMV_XS(scratch, v7); }
            else masm.VMV_XS(scratch, source);
            ++value_stats.cache_hits;
            return scratch;
        }
    }
    if (part && ZeroHigh(value.Def())) return x0;
    Load(scratch, values, static_cast<s64>(value.Def()->Id()) * kValueStride + part * 8);
    ++value_stats.loads;
    return scratch;
}

void JitContext::Data(GPR result, const ir::DataClass& data) {
    if (data.IsValue()) Read(result, data.value);
    else if (data.IsImm()) masm.LI(result, data.imm.Get());
    else masm.MV(result, x0);
}

void JitContext::Operand(GPR result, const ir::Operand& operand) {
    ASSERT(result != t3 && result != t4 && result != t6);
    Data(result, operand.GetLeft());
    if (operand.GetRight().Null() || operand.GetOp().type == ir::OperandOp::None) return;
    Data(t3, operand.GetRight());
    switch (operand.GetOp().type) {
        case ir::OperandOp::Plus: masm.ADD(result, result, t3); break;
        case ir::OperandOp::PlusExt:
            masm.SLLI(t3, t3, operand.GetOp().shift_ext);
            masm.ADD(result, result, t3);
            break;
        case ir::OperandOp::LSL: masm.SLL(result, result, t3); break;
        case ir::OperandOp::LSR:
            if (operand.GetLeft().IsValue())
                Mask(result, ir::GetValueSizeByte(operand.GetLeft().value.Type()) * 8);
            masm.SRL(result, result, t3);
            break;
        default: PANIC("unsupported RV64 operand");
    }
}

void JitContext::Mask(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits == 8) {
        masm.ANDI(value, value, 255);
    } else if (bits < 64) {
        // Biscuit::ZEXTW emits Zba ADD.UW; shifts require only RV64I.
        masm.SLLI(value, value, 64 - bits);
        masm.SRLI(value, value, 64 - bits);
    }
}

void JitContext::SignExtend(GPR value, u32 bits) {
    ASSERT(bits > 0 && bits <= 64);
    if (bits < 64) {
        masm.SLLI(value, value, 64 - bits);
        masm.SRAI(value, value, 64 - bits);
    }
}

GPR JitContext::ResultRegister(ir::Inst* inst) {
    if (!cache_scalars || inst->ReturnType() == ir::ValueType::VOID ||
        ir::IsFloatValueType(inst->ReturnType())) return t0;
    return ReserveRegister();
}

GPR JitContext::ReserveRegister(GPR excluded) {
    const auto count = cached_values.size() - size_t(flags_enabled);
    auto index = next_register;
    while ((reserved_gprs & (1u << index)) || scalar_registers[index] == excluded) index = (index + 1) % count;
    for (size_t scanned = 0; scanned < count; ++scanned) {
        const auto candidate = scanned;
        if (!(reserved_gprs & (1u << candidate)) && !cached_values[candidate] && scalar_registers[candidate] != excluded) { index = candidate; break; }
    }
    next_register = (index + 1) % (cached_values.size() - size_t(flags_enabled));
    if (auto* previous = cached_values[index]) {
        Store(scalar_registers[index], values, static_cast<s64>(previous->Id()) * kValueStride + cached_parts[index] * 8);
        ++value_stats.stores;
        ++value_stats.spills;
    }
    // Evict before operand reads: the selected destination may hold an input
    // to this instruction. Publish the new mapping only after its definition.
    cached_values[index] = nullptr;
    used_gprs |= 1u << index;
    return scalar_registers[index];
}

void JitContext::Write(ir::Inst* inst, GPR value, bool normalized) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    if (!normalized) Mask(value, ir::GetValueSizeByte(inst->ReturnType()) * 8);
    if (cache_scalars) {
        for (size_t i = 0; i < scalar_registers.size(); ++i) {
            if (value == scalar_registers[i]) {
                ASSERT(cached_values[i] == nullptr);
                cached_values[i] = inst;
                cached_parts[i] = 0;
                return;
            }
        }
        PANIC("RV64 scalar result has no reserved register");
    }
    Store(value, values, static_cast<s64>(inst->Id()) * kValueStride);
    ++value_stats.stores;
}

void JitContext::ReleaseDeadValues(u32 position) {
    if (last_uses.empty()) return;
    for (auto& value : cached_values) {
        if (value && last_uses.at(value) <= position) { value = nullptr; ++value_stats.dead_values; }
    }
    for (auto& value : cached_vectors) {
        if (value && last_uses.at(value) <= position) { value = nullptr; ++value_stats.dead_values; }
    }
}

void JitContext::PublishFlags() {
    PublishUniformBindings();
    if (flags_enabled && flags_dirty) {
        Store(flags, state, state_offset_host_flags);
        flags_dirty = false;
    }
}

void JitContext::ReloadFlags() {
    LoadUniformBindings();
    if (flags_enabled) Load(flags, state, state_offset_host_flags);
    flags_dirty = false;
}

void JitContext::FlushValues() {
    for (size_t i = 0; i < cached_values.size(); ++i) {
        if (auto* inst = cached_values[i]) {
            Store(scalar_registers[i], values, static_cast<s64>(inst->Id()) * kValueStride + cached_parts[i] * 8);
            ++value_stats.stores;
        }
    }
    SaveVectorsForCall(false);
    DiscardValues();
}

void JitContext::DiscardValues() {
    cached_values.fill(nullptr);
    cached_vectors.fill(nullptr);
    next_register = next_vector = 0;
    ResetVectorType();
}

void JitContext::Jump(Label& label) {
    // PC-relative pair avoids JAL's +/-1 MiB limit and survives copying to RX.
    masm.LILabel(t5, &label);
    masm.JR(t5);
}

void JitContext::BranchZero(GPR value, Label& label, bool zero) {
    // Only this local skip uses the +/-4 KiB conditional branch encoding.
    Label skip;
    if (zero) masm.BNE(value, x0, &skip);
    else masm.BEQ(value, x0, &skip);
    Jump(label);
    masm.Bind(&skip);
}

void JitContext::Condition(GPR result, ir::Cond condition) {
    ASSERT(result != t1 && result != t2 && result != t3 && result != t4);
    if (condition == ir::Cond::AL || condition == ir::Cond::NV) {
        masm.LI(result, 1);
        return;
    }
    ASSERT(flags_enabled);
    const auto extract = [&](GPR reg, u32 bit) {
        masm.SRLI(reg, flags, bit);
        masm.ANDI(reg, reg, 1);
    };
    // Most guest conditions need only one flag; avoid extracting all NZCV.
    switch (condition) {
        case ir::Cond::EQ: case ir::Cond::NE:
            extract(result, kZeroBit);
            if (condition == ir::Cond::NE) masm.XORI(result, result, 1);
            break;
        case ir::Cond::CS: case ir::Cond::CC:
            extract(result, kCarryBit);
            if (condition == ir::Cond::CC) masm.XORI(result, result, 1);
            break;
        case ir::Cond::MI: case ir::Cond::PL:
            extract(result, kNegateBit);
            if (condition == ir::Cond::PL) masm.XORI(result, result, 1);
            break;
        case ir::Cond::VS: case ir::Cond::VC:
            extract(result, kOverflowBit);
            if (condition == ir::Cond::VC) masm.XORI(result, result, 1);
            break;
        case ir::Cond::HI: case ir::Cond::LS:
            extract(t2, kZeroBit);
            extract(t3, kCarryBit);
            if (condition == ir::Cond::HI) {
                masm.XORI(t2, t2, 1); masm.AND(result, t3, t2);
            } else {
                masm.XORI(t3, t3, 1); masm.OR(result, t3, t2);
            }
            break;
        case ir::Cond::GE: case ir::Cond::LT:
            extract(t1, kNegateBit);
            extract(t4, kOverflowBit);
            masm.XOR(result, t1, t4);
            if (condition == ir::Cond::GE) masm.XORI(result, result, 1);
            break;
        case ir::Cond::GT: case ir::Cond::LE:
            extract(t1, kNegateBit);
            extract(t2, kZeroBit);
            extract(t4, kOverflowBit);
            masm.XOR(result, t1, t4); masm.OR(result, result, t2);
            if (condition == ir::Cond::GT) masm.XORI(result, result, 1);
            break;
        case ir::Cond::AL: case ir::Cond::NV: break;
    }
}

u32 JitContext::CurrentBufferSize() {
    return static_cast<u32>(masm.GetCodeBuffer().GetSizeInBytes());
}

void JitContext::Flush(const CodeBuffer& buffer) {
    ASSERT(buffer.size >= CurrentBufferSize());
    std::memcpy(buffer.rw_data, masm.GetCodeBuffer().GetOffsetPointer(0), CurrentBufferSize());
    buffer.Flush();
}

void JitContext::Prepend(u32 start, std::span<const u8> code) {
    for (auto& ref : host_references) if (ref.offset >= start) ref.offset += code.size();
    for (auto& site : link_sites) if (site.code_offset >= start) site.code_offset += code.size();
    auto& buffer = masm.GetCodeBuffer();
    const auto end = buffer.GetCursorOffset();
    ASSERT(start <= end);
    if (buffer.GetRemainingBytes() <= code.size()) buffer.Grow(buffer.GetSizeInBytes() + code.size() + 4096);
    auto* begin = buffer.GetOffsetPointer(start);
    std::memmove(begin + code.size(), begin, end - start);
    std::memcpy(begin, code.data(), code.size());
    buffer.AdvanceCursor(end + code.size());
}

}  // namespace swift::runtime::backend::riscv64
