#include "emit.h"

namespace swift::runtime::backend::x64::detail {
namespace {
void PinLoad(Xbyak::CodeGenerator& gen, const Xbyak::Reg64& target, unsigned reg, unsigned bits) {
    switch (bits) {
        case 64:
            gen.mov(target, Xbyak::Reg64(reg));
            break;
        case 32:
            gen.mov(Xbyak::Reg32(target.getIdx()), Xbyak::Reg32(reg));
            break;
        case 16:
            gen.movzx(Xbyak::Reg32(target.getIdx()), Xbyak::Reg16(reg));
            break;
        case 8:
            gen.movzx(Xbyak::Reg32(target.getIdx()), Xbyak::Reg8(reg));
            break;
        default:
            break;
    }
}

void PinStore(Xbyak::CodeGenerator& gen, unsigned reg, const Xbyak::Reg64& source, unsigned bits) {
    switch (bits) {
        case 64:
            gen.mov(Xbyak::Reg64(reg), source);
            break;
        case 32:
            gen.mov(Xbyak::Reg32(reg), Xbyak::Reg32(source.getIdx()));
            break;
        case 16:
            gen.mov(Xbyak::Reg16(reg), Xbyak::Reg16(source.getIdx()));
            break;
        case 8:
            gen.mov(Xbyak::Reg8(reg), Xbyak::Reg8(source.getIdx()));
            break;
        default:
            break;
    }
}
}  // namespace

void Writer::Uniform(OpContext& c) {
    auto at = c.operands.begin();
    const auto u = *at++;
    const auto off = u.Payload();
    if (off > INT32_MAX) Reject(CompileError::UnsupportedOperand);
    const auto width = c.operation.opcode == OpCode::LoadUniform ? c.width : TypeBits(u.TypeView());
    if (ArmNativeFP() && off < std::uint64_t{options.arm_fpsr_offset} + 4 &&
        options.arm_fpsr_offset < off + (width + 7) / 8)
        HarvestArmFPStatus();
    const auto bytes = (width + 7) / 8;
    const bool flags_access = options.flags_offset != UINT32_MAX &&
                              off < u64(options.flags_offset) + 8 &&
                              u64(options.flags_offset) < off + bytes;
    if (flags_access) PublishFlags();
    const auto pinned = PinnedSlot(off, bytes);
    const auto relative = pinned >= 0 ? off - options.pinned_uniform_offset - pinned * 8 : -1;
    if (c.operation.opcode == OpCode::LoadUniform) {
        const auto& home = allocation.homes[c.node.value];
        if (home.spilled && home.uniform_offset != UINT32_MAX && !home.uniform_store_bytes &&
            home.auxiliary_offset == UINT32_MAX && !flags_access && pinned < 0 &&
            (!ArmNativeFP() || !(off < std::uint64_t{options.arm_fpsr_offset} + 4 &&
                                 options.arm_fpsr_offset < off + bytes))) {
            c.saved = true;
            return;
        }
        const auto destination = ScalarDestination(c);
        if (c.vector_result) {
            if (!home.spilled) c.vector = Xmm(home.reg);
            if (options.avx)
                vmovdqu(c.vector, ptr[r12 + off]);
            else
                movdqu(c.vector, ptr[r12 + off]);
        } else if (off == options.flags_offset)
            mov(destination, rbx);
        else if (pinned >= 0 && relative == 0 &&
                 (width == 64 || width == 32 || width == 16 || width == 8))
            PinLoad(*this, destination, PinnedRegister(static_cast<unsigned>(pinned)), width);
        else if (pinned >= 0) {
            // Rare in-slot access (for example AH): publish this slot, read
            // memory, then reload the register copy.
            mov(qword[r12 + options.pinned_uniform_offset + pinned * 8],
                Reg64(PinnedRegister(static_cast<unsigned>(pinned))));
            Load(destination, r12, off, c.width);
            mov(Reg64(PinnedRegister(static_cast<unsigned>(pinned))),
                qword[r12 + options.pinned_uniform_offset + pinned * 8]);
        } else
            Load(destination, r12, off, c.width);
        c.scalar_normalized = off != options.flags_offset && width >= 8;
        return;
    }
    const auto value = *at;
    if (value.Kind() == OperandKind::Value) {
        const auto& home = allocation.homes[value.Payload()];
        if (home.spilled && home.uniform_store_bytes && home.uniform_offset == off) return;
    }
    if (View(value) == ir::Type::V128) {
        if (value.Kind() == OperandKind::Value &&
            source.Operations()[value.Payload()].type == ir::Type::V128 &&
            !allocation.homes[value.Payload()].spilled) {
            if (options.avx)
                vmovdqu(ptr[r12 + off], Xmm(allocation.homes[value.Payload()].reg));
            else
                movdqu(ptr[r12 + off], Xmm(allocation.homes[value.Payload()].reg));
        } else {
            ReadVector(value, xmm0);
            if (options.avx)
                vmovdqu(ptr[r12 + off], xmm0);
            else
                movdqu(ptr[r12 + off], xmm0);
        }
    } else {
        std::int32_t literal{};
        bool has_literal = Immediate32(value, width, literal);
        if (!has_literal && value.Kind() == OperandKind::Value &&
            allocation.homes[value.Payload()].rematerialize && width >= 8 && width <= 64) {
            u64 imm = source.Operands(NodeId{u32(value.Payload())}).front().Payload();
            const auto nbits = TypeBits(source.Operations()[value.Payload()].type);
            if (nbits && nbits < 64) imm &= (u64{1} << nbits) - 1;
            const auto vbits = TypeBits(View(value));
            if (vbits && vbits < 64) imm &= (u64{1} << vbits) - 1;
            literal = static_cast<std::int32_t>(imm);
            has_literal = width <= 32 || imm == u64(std::int64_t(literal));
        }
        if (pinned < 0 && !flags_access && width >= 8 && has_literal) {
            if (width == 8)
                mov(byte[r12 + off], literal);
            else if (width == 16)
                mov(word[r12 + off], literal);
            else if (width == 32)
                mov(dword[r12 + off], literal);
            else
                mov(qword[r12 + off], literal);
        } else {
            Reg64 input = rax;
            // A store consumes only its declared low bits. A resident scalar can
            // supply them directly without copying through RAX or changing state.
            if (value.Kind() == OperandKind::Value && width >= 8 && pinned < 0 &&
                off != options.flags_offset && width <= TypeBits(View(value)) &&
                source.Operations()[value.Payload()].type != ir::Type::V128 &&
                !allocation.homes[value.Payload()].spilled)
                input = Reg64(allocation.homes[value.Payload()].reg);
            else
                Read(value, input);
            if (pinned >= 0 && relative == 0 &&
                (width == 64 || width == 32 || width == 16 || width == 8))
                PinStore(*this, PinnedRegister(static_cast<unsigned>(pinned)), input, width);
            else if (pinned >= 0) {
                mov(qword[r12 + options.pinned_uniform_offset + pinned * 8],
                    Reg64(PinnedRegister(static_cast<unsigned>(pinned))));
                Store(input, r12, off, TypeBits(u.TypeView()));
                mov(Reg64(PinnedRegister(static_cast<unsigned>(pinned))),
                    qword[r12 + options.pinned_uniform_offset + pinned * 8]);
            } else
                Store(input, r12, off, TypeBits(u.TypeView()));
            if (off == options.flags_offset && width == 64) mov(rbx, input);
        }
    }
    if (off == options.pc_offset) {
        static_pc_known = false;
        if (value.Kind() == OperandKind::Value) {
            const auto& op = source.Operations()[value.Payload()];
            if (op.opcode == OpCode::LoadImm) {
                static_pc = source.Operands(NodeId{u32(value.Payload())}).front().Payload();
                static_pc_known = true;
            }
        }
        // Unknown/EarlyOff frontends publish PC explicitly instead of
        // emitting FaultLocation. Those stores still identify the memory
        // accesses whose native faults the execution owner must recover.
        if (static_pc_known) {
            guest_pc = static_pc;
            has_fault_location = true;
        }
    }
    if (flags_access && (off != options.flags_offset || width != 64))
        mov(rbx, qword[r12 + options.flags_offset]);
}

void Writer::Handle(Op<OpCode::LoadUniform>, OpContext& c) { Uniform(c); }

void Writer::Handle(Op<OpCode::StoreUniform>, OpContext& c) { Uniform(c); }

void Writer::Handle(Op<OpCode::GetUniformAddress>, OpContext& c) {
    lea(ScalarDestination(c), ptr[r12 + c.Arg(0).Payload()]);
}

void Writer::MemoryAddress(CellRange::Iterator& at,
                           CellRange::Iterator end,
                           Reg64& base,
                           std::int32_t& disp) {
    base = r11;
    disp = 0;
    if (at == end) Reject(CompileError::UnsupportedOperand);
    const auto first = *at;
    if (first.Kind() == OperandKind::Value && TypeBits(View(first)) == 64 &&
        source.Operations()[first.Payload()].type != ir::Type::V128) {
        const auto addr_id = static_cast<u32>(first.Payload());
        const auto& ah = allocation.homes[addr_id];
        const auto& aop = source.Operations()[addr_id];
        if (ah.rematerialize && (aop.opcode == OpCode::Add || aop.opcode == OpCode::Sub)) {
            auto ait = source.Operands(NodeId{addr_id}).begin();
            const auto base_cell = *ait++, imm_cell = *ait;
            u64 val = imm_cell.Payload();
            const auto ibits = TypeBits(imm_cell.TypeView() != ir::Type::Void
                                                ? imm_cell.TypeView()
                                                : ir::Type::U64);
            if (ibits && ibits < 64) val &= (u64{1} << ibits) - 1;
            const u64 d64 = aop.opcode == OpCode::Sub ? (u64{0} - val) : val;
            disp = static_cast<std::int32_t>(d64);
            const auto& bh = allocation.homes[base_cell.Payload()];
            if (!bh.spilled && !(in_call && bh.reg >= 6 && bh.reg <= 9))
                base = Reg64(bh.reg);
            else
                Read(base_cell, base);
            ++at;
            return;
        }
        if (!ah.spilled && !(in_call && ah.reg >= 6 && ah.reg <= 9)) {
            base = Reg64(ah.reg);
            ++at;
            return;
        }
    } else if (first.Kind() == OperandKind::Expression && first.Count() == 2 &&
               (first.ExpressionOp() == OperandOp::Add ||
                first.ExpressionOp() == OperandOp::Sub)) {
        auto probe = at;
        ++probe;
        if (probe != end) {
            const auto base_cell = *probe++;
            if (probe != end) {
                const auto imm_cell = *probe++;
                if (base_cell.Kind() == OperandKind::Value && TypeBits(View(base_cell)) == 64 &&
                    source.Operations()[base_cell.Payload()].type != ir::Type::V128 &&
                    imm_cell.Kind() == OperandKind::Immediate) {
                    u64 val = imm_cell.Payload();
                    const auto ibits = TypeBits(imm_cell.TypeView() != ir::Type::Void
                                                        ? imm_cell.TypeView()
                                                        : ir::Type::U64);
                    if (ibits && ibits < 64) val &= (u64{1} << ibits) - 1;
                    const u64 d64 = first.ExpressionOp() == OperandOp::Sub ? (u64{0} - val) : val;
                    const auto d32 = static_cast<std::int32_t>(d64);
                    if (d64 == u64(std::int64_t(d32))) {
                        disp = d32;
                        const auto& bh = allocation.homes[base_cell.Payload()];
                        if (!bh.spilled && !(in_call && bh.reg >= 6 && bh.reg <= 9))
                            base = Reg64(bh.reg);
                        else
                            Read(base_cell, base);
                        at = probe;
                        return;
                    }
                }
            }
        }
    }
    Expression(at, end, base);
}

void Writer::Memory(OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    const bool load =
            c.operation.opcode == OpCode::LoadMemory || c.operation.opcode == OpCode::LoadMemoryTSO;
    auto at = c.operands.begin();
    Reg64 address = r11;
    std::int32_t disp = 0;
    MemoryAddress(at, c.operands.end(), address, disp);
    if (load) {
        const auto first = static_cast<u32>(getSize());
        if (c.vector_result) {
            const auto& home = allocation.homes[c.node.value];
            if (!home.spilled) c.vector = Xmm(home.reg);
            if (options.avx)
                vmovdqu(c.vector, ptr[address + disp]);
            else
                movdqu(c.vector, ptr[address + disp]);
        } else {
            Load(ScalarDestination(c), address, disp, c.width);
            c.scalar_normalized = c.width >= 8;
        }
        RecordGuestAccess(first, 1, c.vector_result ? 16 : c.width / 8);
    } else {
        const auto v = *at;
        if (View(v) == ir::Type::V128) {
            Xmm input = xmm0;
            if (v.Kind() == OperandKind::Value &&
                source.Operations()[v.Payload()].type == ir::Type::V128 &&
                !allocation.homes[v.Payload()].spilled)
                input = Xmm(allocation.homes[v.Payload()].reg);
            else
                ReadVector(v, input);
            const auto first = static_cast<u32>(getSize());
            if (options.avx)
                vmovdqu(ptr[address + disp], input);
            else
                movdqu(ptr[address + disp], input);
            RecordGuestAccess(first, 2, 16);
        } else {
            Reg64 input = rax;
            const auto bits = TypeBits(View(v));
            if (v.Kind() == OperandKind::Value && bits >= 8 &&
                source.Operations()[v.Payload()].type != ir::Type::V128 &&
                !allocation.homes[v.Payload()].spilled)
                input = Reg64(allocation.homes[v.Payload()].reg);
            else
                Read(v, input, true);
            const auto first = static_cast<u32>(getSize());
            Store(input, address, disp, bits);
            RecordGuestAccess(first, 2, TypeBits(View(v)) / 8);
        }
    }
}

#define MEM(name)                                                                                  \
    void Writer::Handle(Op<OpCode::name>, OpContext& c) { Memory(c); }
// clang-format off
MEM(LoadMemory)
MEM(LoadMemoryTSO)
MEM(StoreMemory)
MEM(StoreMemoryTSO)
// clang-format on
#undef MEM

void Writer::Handle(Op<OpCode::MemoryBarrierTSO>, OpContext&) { mfence(); }

void Writer::Handle(Op<OpCode::MemoryStoreFence>, OpContext&) { sfence(); }

void Writer::Handle(Op<OpCode::MemoryLoadFence>, OpContext&) { lfence(); }

void Writer::Handle(Op<OpCode::CpuYield>, OpContext&) { pause(); }

void Writer::Handle(Op<OpCode::CheckMemoryAlignment>, OpContext& c) {
    Read(c.Arg(0), rax);
    mov(rcx, c.Arg(1).Payload());
    test(rax, rcx);
    Label ok;
    jz(ok, T_NEAR);
    Exit(ExitValue(NativeExit::AlignmentFault));
    L(ok);
}

void Writer::Handle(Op<OpCode::StoreUniformSelectImm>, OpContext& c) {
    const auto& body = source.Blocks()[block.value];
    if (Linked() && c.Arg(0).Payload() == options.pc_offset &&
        c.node.value + 1 == body.first_node + body.node_count &&
        body.terminal.kind == TerminalKind::Dispatch) {
        Read(c.Arg(1), rax);
        test(rax, rax);
        Label no;
        jz(no, T_NEAR);
        StaticExit(c.Arg(2).Payload());
        L(no);
        StaticExit(c.Arg(3).Payload());
        terminated = true;
        return;
    }
    Read(c.Arg(1), rax);
    test(rax, rax);
    mov(rax, c.Arg(3).Payload());
    mov(rdx, c.Arg(2).Payload());
    cmovnz(rax, rdx);
    const auto select_off = c.Arg(0).Payload();
    const auto select_pin = PinnedSlot(select_off, 8);
    if (select_pin >= 0)
        mov(qword[r12 + options.pinned_uniform_offset + select_pin * 8],
            Reg64(PinnedRegister(static_cast<unsigned>(select_pin))));
    Store(rax, r12, select_off, 64);
    if (select_pin >= 0)
        mov(Reg64(PinnedRegister(static_cast<unsigned>(select_pin))),
            qword[r12 + options.pinned_uniform_offset + select_pin * 8]);
    if (c.Arg(0).Payload() == options.pc_offset) static_pc_known = false;
}
}  // namespace swift::runtime::backend::x64::detail
