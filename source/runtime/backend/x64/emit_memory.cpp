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
            allocation.homes[value.Payload()].rematerialize &&
            source.Operations()[value.Payload()].opcode == OpCode::LoadImm && width >= 8 &&
            width <= 64) {
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

Xbyak::RegExp Writer::MemoryAddress(CellRange::Iterator& at, CellRange::Iterator end) {
    if (at == end) Reject(CompileError::UnsupportedOperand);
    const auto const_disp32 = [&](CellView imm_cell, bool negate, std::int32_t& d_out) -> bool {
        if (imm_cell.Kind() != OperandKind::Immediate) return false;
        u64 val = imm_cell.Payload();
        const auto ibits =
                TypeBits(imm_cell.TypeView() != ir::Type::Void ? imm_cell.TypeView() : ir::Type::U64);
        if (ibits && ibits < 64) val &= (u64{1} << ibits) - 1;
        const u64 d64 = negate ? (u64{0} - val) : val;
        const auto d32 = static_cast<std::int32_t>(d64);
        if (d64 != u64(std::int64_t(d32))) return false;
        d_out = d32;
        return true;
    };
    const auto is_u64_val = [&](CellView c) -> bool {
        return c.Kind() == OperandKind::Value && TypeBits(View(c)) == 64 &&
               source.Operations()[c.Payload()].type != ir::Type::V128;
    };
    const auto load_base_reg = [&](CellView c) -> Reg64 {
        const auto& bh = allocation.homes[c.Payload()];
        if (!bh.spilled && !(in_call && bh.reg >= 6 && bh.reg <= 9)) return Reg64(bh.reg);
        Read(c, r11);
        return r11;
    };
    const auto load_index_reg = [&](CellView c, bool uxtw, CellView base_c, Reg64 base_reg) -> Reg64 {
        const auto id = static_cast<u32>(c.Payload());
        const auto& ih = allocation.homes[id];
        const auto inat = source.Operations()[id].type;
        if (!uxtw) {
            if (id == base_c.Payload()) return base_reg;
            if (!ih.spilled && !(in_call && ih.reg >= 6 && ih.reg <= 9)) return Reg64(ih.reg);
            Read(c, r10);
            return r10;
        }
        if (!ih.spilled && !(in_call && ih.reg >= 6 && ih.reg <= 9)) {
            if (inat == ir::Type::U32) return Reg64(ih.reg);
            mov(r10d, Reg32(ih.reg));
            return r10;
        }
        if (ih.spilled && ih.uniform_offset != UINT32_MAX) {
            mov(r10d, dword[r12 + ih.uniform_offset]);
            return r10;
        }
        if (ih.rematerialize && source.Operations()[id].opcode == OpCode::LoadImm) {
            mov(r10d, u32(source.Operands(NodeId{id}).front().Payload()));
            return r10;
        }
        const auto off = ih.spilled ? ih.stack_offset : allocation.call_offset + 192 + (ih.reg - 6) * 8;
        mov(r10d, dword[rsp + off]);
        return r10;
    };

    const auto first = *at;
    if (is_u64_val(first)) {
        CellView base_cell{}, index_cell{};
        bool has_base = false, has_index = false, index_uxtw = false;
        unsigned scale = 1;
        std::int64_t disp64 = 0;
        const auto is_fused = [&](u32 nid) -> bool {
            const auto& nop = source.Operations()[nid];
            return allocation.homes[nid].rematerialize && nop.opcode != OpCode::LoadImm &&
                   nop.opcode != OpCode::LoadMemory && nop.opcode != OpCode::LoadMemoryTSO;
        };
        const auto walk = [&](auto& self, CellView c) -> void {
            const u32 nid = static_cast<u32>(c.Payload());
            if (!is_fused(nid)) {
                if (!has_base) {
                    has_base = true;
                    base_cell = c;
                } else {
                    has_index = true;
                    index_cell = c;
                    index_uxtw = false;
                    scale = 1;
                }
                return;
            }
            const auto& nop = source.Operations()[nid];
            if (nop.opcode == OpCode::ZeroExtend32To64 || nop.opcode == OpCode::ZeroExtend32 ||
                nop.opcode == OpCode::And) {
                has_index = true;
                index_cell = *source.Operands(NodeId{nid}).begin();
                index_uxtw = true;
                scale = 1;
                return;
            }
            if (nop.opcode == OpCode::LslImm) {
                auto it = source.Operands(NodeId{nid}).begin();
                const auto idx = *it++, sh = *it;
                has_index = true;
                scale = 1u << (sh.Payload() & 63);
                const u32 idx_id = static_cast<u32>(idx.Payload());
                if (is_fused(idx_id)) {
                    index_cell = *source.Operands(NodeId{idx_id}).begin();
                    index_uxtw = true;
                } else {
                    index_cell = idx;
                    index_uxtw = false;
                }
                return;
            }
            if (nop.opcode == OpCode::Add && nop.operand_count == 3) {
                auto it = source.Operands(NodeId{nid}).begin();
                const auto lhs = *it++, expr = *it++, idx = *it;
                has_index = true;
                index_cell = idx;
                index_uxtw = false;
                scale = 1u << expr.ShiftAmount();
                self(self, lhs);
                return;
            }
            auto it = source.Operands(NodeId{nid}).begin();
            const auto lhs = *it++, rhs = *it;
            std::int32_t d32 = 0;
            if (const_disp32(rhs, nop.opcode == OpCode::Sub, d32)) {
                disp64 += d32;
                self(self, lhs);
            } else {
                self(self, lhs);
                self(self, rhs);
            }
        };
        walk(walk, first);
        ++at;
        const Reg64 base_reg = load_base_reg(base_cell);
        Xbyak::RegExp exp = base_reg;
        if (has_index) {
            const Reg64 idx_reg = load_index_reg(index_cell, index_uxtw, base_cell, base_reg);
            exp = exp + idx_reg * scale;
        }
        if (disp64) exp = exp + static_cast<std::int32_t>(disp64);
        return exp;
    }

    if (first.Kind() == OperandKind::Expression && first.Count() == 2) {
        auto probe = at;
        ++probe;
        const auto parse_sib_pair = [&](CellView hdr,
                                        CellRange::Iterator& p,
                                        CellView& b_out,
                                        CellView& i_out,
                                        unsigned& sc_out) -> bool {
            if (hdr.Kind() != OperandKind::Expression || hdr.Count() != 2) return false;
            if (hdr.ExpressionOp() != OperandOp::Add &&
                !(hdr.ExpressionOp() == OperandOp::AddShift && hdr.ShiftAmount() <= 3))
                return false;
            if (p == end) return false;
            const auto b_cell = *p++;
            if (p == end) return false;
            const auto i_cell = *p++;
            if (!is_u64_val(b_cell) || !is_u64_val(i_cell)) return false;
            b_out = b_cell;
            i_out = i_cell;
            sc_out = hdr.ExpressionOp() == OperandOp::AddShift ? (1u << hdr.ShiftAmount()) : 1u;
            return true;
        };

        CellView b_cell{}, i_cell{};
        unsigned sc = 1;
        if (parse_sib_pair(first, probe, b_cell, i_cell, sc)) {
            at = probe;
            const Reg64 b_reg = load_base_reg(b_cell);
            const Reg64 i_reg = load_index_reg(i_cell, false, b_cell, b_reg);
            return b_reg + i_reg * sc;
        }
        if ((first.ExpressionOp() == OperandOp::Add || first.ExpressionOp() == OperandOp::Sub) &&
            probe != end) {
            const auto sub_first = *probe++;
            if (is_u64_val(sub_first) && probe != end) {
                const auto imm_cell = *probe++;
                std::int32_t d32 = 0;
                if (const_disp32(imm_cell, first.ExpressionOp() == OperandOp::Sub, d32)) {
                    at = probe;
                    const Reg64 b_reg = load_base_reg(sub_first);
                    return d32 ? (b_reg + d32) : Xbyak::RegExp(b_reg);
                }
            } else if (parse_sib_pair(sub_first, probe, b_cell, i_cell, sc) && probe != end) {
                const auto imm_cell = *probe++;
                std::int32_t d32 = 0;
                if (const_disp32(imm_cell, first.ExpressionOp() == OperandOp::Sub, d32)) {
                    at = probe;
                    const Reg64 b_reg = load_base_reg(b_cell);
                    const Reg64 i_reg = load_index_reg(i_cell, false, b_cell, b_reg);
                    Xbyak::RegExp exp = b_reg + i_reg * sc;
                    if (d32) exp = exp + d32;
                    return exp;
                }
            }
        }
    }

    Expression(at, end, r11);
    return Xbyak::RegExp(r11);
}

namespace {
void SkipAddressOperand(CellRange::Iterator& it, CellRange::Iterator end) {
    if (it == end) return;
    const auto cell = *it++;
    if (cell.Kind() == OperandKind::Expression)
        for (unsigned i = 0; i < cell.Count(); ++i) SkipAddressOperand(it, end);
}
}  // namespace

void Writer::Memory(OpContext& c) {
    if (allocation.homes[c.node.value].rematerialize) {
        c.saved = true;
        return;
    }
    const bool load =
            c.operation.opcode == OpCode::LoadMemory || c.operation.opcode == OpCode::LoadMemoryTSO;
    auto at = c.operands.begin();
    if (load) {
        const auto addr = MemoryAddress(at, c.operands.end());
        const auto first = static_cast<u32>(getSize());
        if (c.vector_result) {
            const auto& home = allocation.homes[c.node.value];
            if (!home.spilled) c.vector = Xmm(home.reg);
            if (options.avx)
                vmovdqu(c.vector, ptr[addr]);
            else
                movdqu(c.vector, ptr[addr]);
        } else {
            Load(ScalarDestination(c), addr, c.width);
            c.scalar_normalized = c.width >= 8;
        }
        RecordGuestAccess(first, 1, c.vector_result ? 16 : c.width / 8);
    } else {
        auto val_it = c.operands.begin();
        SkipAddressOperand(val_it, c.operands.end());
        if (val_it == c.operands.end()) Reject(CompileError::UnsupportedOperand);
        const auto v = *val_it;
        if (View(v) == ir::Type::V128) {
            Xmm input = xmm0;
            if (v.Kind() == OperandKind::Value &&
                source.Operations()[v.Payload()].type == ir::Type::V128 &&
                !allocation.homes[v.Payload()].spilled)
                input = Xmm(allocation.homes[v.Payload()].reg);
            else
                ReadVector(v, input);
            const auto addr = MemoryAddress(at, c.operands.end());
            const auto first = static_cast<u32>(getSize());
            if (options.avx)
                vmovdqu(ptr[addr], input);
            else
                movdqu(ptr[addr], input);
            RecordGuestAccess(first, 2, 16);
        } else {
            Reg64 input = rax;
            const auto bits = TypeBits(View(v));
            if (v.Kind() == OperandKind::Value && bits >= 8 &&
                source.Operations()[v.Payload()].type != ir::Type::V128 &&
                !allocation.homes[v.Payload()].spilled &&
                !(in_call && allocation.homes[v.Payload()].reg >= 6 &&
                  allocation.homes[v.Payload()].reg <= 9))
                input = Reg64(allocation.homes[v.Payload()].reg);
            else
                Read(v, input, true);
            const auto addr = MemoryAddress(at, c.operands.end());
            const auto first = static_cast<u32>(getSize());
            Store(input, addr, bits);
            RecordGuestAccess(first, 2, bits / 8);
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
