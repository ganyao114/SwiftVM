#include "abi_layout.inc"
#include "emit.h"
#include "runtime/exit.h"

#include <algorithm>

namespace swift::runtime::backend::loongarch64::detail {
namespace {
struct UnitLbtPlan {
    bool canonical_blocks{};
    u32 used_mask{};
    u32 written_mask{};
};

UnitLbtPlan AnalyzeUnitLbt(const Program& source, const CompileOptions& options) {
    UnitLbtPlan plan{};
    if (!options.lbt_x86 || source.Blocks().empty()) return plan;
    bool has_local_branch = false;
    bool uses_flags = false;
    const auto ops = source.Operations();
    for (u32 idx = 0; idx < ops.size(); ++idx) {
        const auto& op = ops[idx];
        if (op.flag_outputs) {
            uses_flags = true;
            plan.written_mask |= LbtFlagMask(op.flag_outputs);
        }
        const auto operands = source.Operands(NodeId{idx});
        auto it = operands.begin();
        const u64 arg0 = it != operands.end() ? (*it++).Payload() : 0;
        const u64 arg1 = it != operands.end() ? (*it++).Payload() : 0;
        switch (op.opcode) {
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel:
                has_local_branch = true;
                break;
            case OpCode::Adc:
            case OpCode::Sbb:
                uses_flags = true;
                plan.used_mask |= 0x01u;
                break;
            case OpCode::CondSet:
            case OpCode::LocalCondSet:
            case OpCode::CondSelect:
                uses_flags = true;
                plan.used_mask |= 0x39u;
                break;
            case OpCode::GetFlags:
                uses_flags = true;
                break;
            case OpCode::RestoreFlags:
            case OpCode::SaveFlags:
            case OpCode::BranchOnlyFlags:
            case OpCode::PublishSse42StrFlags:
                uses_flags = true;
                plan.written_mask |= LbtFlagMask(arg1);
                break;
            case OpCode::ClearFlags:
                uses_flags = true;
                plan.written_mask |= LbtFlagMask(arg0);
                break;
            case OpCode::InvertCarry:
                uses_flags = true;
                plan.used_mask |= 0x01u;
                plan.written_mask |= 0x01u;
                break;
            case OpCode::SetCarry:
                uses_flags = true;
                plan.written_mask |= 0x01u;
                break;
            case OpCode::SetOverflow:
                uses_flags = true;
                plan.written_mask |= 0x20u;
                break;
            case OpCode::TestFlags:
            case OpCode::TestNotFlags:
                uses_flags = true;
                plan.used_mask |= LbtFlagMask(arg0);
                break;
            case OpCode::PublishFCmpFlags:
                uses_flags = true;
                plan.written_mask |= 0x3fu;
                break;
            case OpCode::LoadUniform:
                if (arg0 == options.flags_offset) uses_flags = true;
                break;
            case OpCode::StoreUniform:
            case OpCode::StoreUniformSelectImm:
                if (arg0 == options.flags_offset) uses_flags = true;
                break;
            default:
                break;
        }
    }
    plan.used_mask = (plan.used_mask | plan.written_mask) & 0x3fu;
    const auto& entry_term = source.Blocks().front().terminal;
    const bool has_cfg_edges = source.Blocks().size() > 1 ||
                               entry_term.kind == TerminalKind::Jump ||
                               entry_term.kind == TerminalKind::Branch || has_local_branch;
    plan.canonical_blocks = has_cfg_edges && uses_flags;
    return plan;
}
}  // namespace

Writer::Writer(const Program& source,
               const Allocation& allocation,
               CompileResult& output,
               CompileOptions options)
        : source(source)
        , allocation(allocation)
        , output(output)
        , options(options)
        , as(output.words)
        , guest_pc(options.entry_pc)
        , block_offsets(source.Blocks().size(), UINT32_MAX)
        , local_offsets(source.Operations().size(), UINT32_MAX) {
    const auto plan = AnalyzeUnitLbt(source, this->options);
    lbt_canonical_blocks = plan.canonical_blocks;
    unit_lbt_used_mask = plan.used_mask;
    unit_lbt_written_mask = plan.written_mask;
}

void Writer::Reject(CompileError error) { throw Rejected{error, current, block}; }

void Writer::FrameDelta(std::int64_t delta) {
    if (!delta) return;
    if (delta >= -2048 && delta <= 2047)
        as.ADDI(sp, sp, static_cast<int>(delta));
    else {
        as.LI(t6, static_cast<u64>(delta));
        as.ADD(sp, sp, t6);
    }
}

void Writer::Prologue() {
    FrameDelta(-std::int64_t{output.frame_bytes});
    if (options.dispatcher && options.exit_handler) {
        if (lbt_canonical_blocks) ImportPackedToLBT();
        return;
    }
    const auto base = allocation.spill_bytes;
    FrameStore(ra, base);
    FrameStore(state, base + 8);
    for (unsigned i = 0; i != 9; ++i) FrameStore(GPR{23 + i}, base + 16 + 8 * i);
    as.FRCSR(t0);
    FrameStore(t0, base + 96);
    as.MV(state, a0);
    if (options.flags_offset != UINT32_MAX)
        Load(packed_flags, state, options.flags_offset, 64);
    else
        as.MV(packed_flags, x0);
    if (options.mxcsr_offset != UINT32_MAX) {
        Load(t0, state, options.mxcsr_offset, 32);
        as.SRLI(t0, t0, 13);
        as.ANDI(t0, t0, 3);
        // Map architectural rounding controls to the LA FCSR.RM field.
        as.LI(t1, 0x6c);
        as.SLLI(t0, t0, 1);
        as.SRL(t1, t1, t0);
        as.ANDI(t1, t1, 3);
        as.SLLI(t1, t1, 8);
        as.FSCSR(t1);
    }
    if (lbt_canonical_blocks) ImportPackedToLBT();
}

void Writer::Restore() {
    if (!(options.dispatcher && options.exit_handler)) {
        const auto base = allocation.spill_bytes;
        FrameLoad(t0, base + 96);
        as.FSCSR(t0);
        for (unsigned i = 0; i != 9; ++i) FrameLoad(GPR{23 + i}, base + 16 + 8 * i);
        FrameLoad(state, base + 8);
        FrameLoad(ra, base);
    }
    FrameDelta(output.frame_bytes);
}

void Writer::Exit(u64 reason) {
    if (reason >= ExitValue(NativeExit::UnsupportedAtomic) &&
        reason <= ExitValue(NativeExit::FloatingFault) && options.pc_offset != UINT32_MAX) {
        as.LI(t0, guest_pc);
        Store(t0, state, options.pc_offset, 64);
    }
    if (lbt_live) EmitExportLBTToPacked(packed_flags);
    if (options.flags_offset != UINT32_MAX) Store(packed_flags, state, options.flags_offset, 64);
    Restore();
    as.LI(a0, reason);
    if (options.dispatcher && options.exit_handler) {
        HostAddress(t6, options.exit_handler, HostSymbol::Exit);
        as.JR(t6);
    } else
        as.RET();
}

void Writer::StaticExit(u64 pc) {
    if (lbt_live) EmitExportLBTToPacked(packed_flags);
    if (options.flags_offset != UINT32_MAX) Store(packed_flags, state, options.flags_offset, 64);
    Restore();
    as.LI(a0, pc);
    if (output.words.size() & 1) as.NOP();
    const auto branch = static_cast<u32>(output.words.size() * 4);
    as.AUIPC(t6, 0);
    as.LD(t5, 16, t6);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    as.JR(t5);
    const auto data = static_cast<u32>(output.words.size() * 4);
    for (unsigned i = 0; i != 4; ++i) as.GetCodeBuffer().Emit32(0);
    const auto fallback = static_cast<u32>(output.words.size() * 4);
    HostAddress(t6, options.dispatcher, HostSymbol::Dispatcher);
    as.JR(t6);
    output.links.push_back({LinkKind::Static, data, branch, fallback, pc});
}

void Writer::HostAddress(GPR destination,
                         std::uintptr_t address,
                         HostSymbol symbol,
                         std::int64_t addend) {
    if (!options.relocatable) {
        as.LI(destination, address);
        return;
    }
    // The load and its aligned literal are local to this real address use.
    // J skips the data, so no global literal pool or end-of-unit scan exists.
    if (output.words.size() & 1) as.NOP();
    const auto offset = static_cast<NativeOffset>(output.words.size() * 4 + 16);
    as.AUIPC(destination, 0);
    as.LD(destination, 16, destination);
    as.J(16);
    as.NOP();
    as.GetCodeBuffer().Emit32(static_cast<u32>(address));
    as.GetCodeBuffer().Emit32(static_cast<u32>(address >> 32));
    if (symbol == HostSymbol::None) symbol = CallbackSymbol(address);
    if (symbol == HostSymbol::None) output.relocatable = false;
    if (output.relocatable)
        output.relocations.push_back({offset, RelocationKind::Absolute64, symbol, addend});
}

void Writer::Call(std::uintptr_t target) { Call(target, HostSymbol::None); }

void Writer::Call(std::uintptr_t target, HostSymbol symbol) {
    HostAddress(t5, target, symbol);
    CallRegister(t5);
}

void Writer::CallRegister(GPR target) {
    if (options.dispatcher && options.exit_handler) {
        as.LI(t6, SVM_NATIVE_PHASE_HELPER);
        Store(t6, state, SVM_LA_CTX_REL(SVM_LA_CTX_PHASE), 64);
    }
    if (lbt_live) EmitExportLBTToPacked(packed_flags);
    if (options.flags_offset != UINT32_MAX) Store(packed_flags, state, options.flags_offset, 64);
    if (options.native_fp_environment && options.dispatcher && options.exit_handler) {
        as.FRCSR(t6);
        as.LI(t0, 0x1f0000);
        as.AND(t6, t6, t0);
        Store(t6, state, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FPSR), 64);
        Load(t6, state, SVM_LA_CTX_REL(SVM_LA_CTX_HOST_FCSR), 64);
        as.FSCSR(t6);
    } else if (!(options.dispatcher && options.exit_handler)) {
        FrameLoad(t6, allocation.spill_bytes + 96);
        as.FSCSR(t6);
    }
    as.JALR(ra, 0, target);
    if (options.flags_offset != UINT32_MAX) Load(packed_flags, state, options.flags_offset, 64);
    if (lbt_live && unit_lbt_used_mask) {
        EmitPackedToLbt(packed_flags, unit_lbt_used_mask);
        if (carry_xor && (unit_lbt_used_mask & 0x01u)) as.XORI(t7, t7, 1);
        as.X86MTFLAG(t7, unit_lbt_used_mask);
    }
    if (options.native_fp_environment && options.dispatcher && options.exit_handler) {
        Load(t6, state, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FCSR), 64);
        Load(t0, state, SVM_LA_CTX_REL(SVM_LA_CTX_GUEST_FPSR), 64);
        as.OR(t6, t6, t0);
        as.FSCSR(t6);
    } else if (options.mxcsr_offset != UINT32_MAX) {
        Load(t6, state, options.mxcsr_offset, 32);
        as.SRLI(t6, t6, 13);
        as.ANDI(t6, t6, 3);
        // t0 is caller-saved and does not carry the helper return.
        as.LI(t0, 0x6c);
        as.SLLI(t6, t6, 1);
        as.SRL(t0, t0, t6);
        as.ANDI(t0, t0, 3);
        as.SLLI(t0, t0, 8);
        as.FSCSR(t0);
    }
    if (options.dispatcher && options.exit_handler) {
        as.LI(t6, SVM_NATIVE_PHASE_GUEST);
        Store(t6, state, SVM_LA_CTX_REL(SVM_LA_CTX_PHASE), 64);
    }
}

void Writer::JumpTo(u32 target, bool local) {
    jumps.push_back(
            {static_cast<u32>(output.words.size() * 4), target, x0, JumpKind::Unconditional, local});
    as.NOP();
}

void Writer::BranchZero(GPR reg, u32 target, bool local) {
    jumps.push_back({static_cast<u32>(output.words.size() * 4), target, reg, JumpKind::Zero, local});
    as.NOP();
}

void Writer::BranchNonZero(GPR reg, u32 target, bool local) {
    jumps.push_back(
            {static_cast<u32>(output.words.size() * 4), target, reg, JumpKind::NonZero, local});
    as.NOP();
}

void Writer::Terminal(const StoredTerminal& terminal, u32 fallthrough_block) {
    switch (terminal.kind) {
        case TerminalKind::Jump:
            if (lbt_canonical_blocks) NormalizeLBT();
            if (terminal.first.value != fallthrough_block) JumpTo(terminal.first.value);
            return;
        case TerminalKind::Branch: {
            if (lbt_canonical_blocks) NormalizeLBT();
            const auto& home = allocation.homes[terminal.value.value];
            GPR cond_reg = a0;
            if (home.spilled) {
                FrameLoad(a0, home.stack_offset);
            } else if (TypeBits(terminal.value_type) >= 64) {
                cond_reg = GPR{home.reg};
            } else {
                as.MV(a0, GPR{home.reg});
            }
            Mask(cond_reg, TypeBits(terminal.value_type));
            const u32 yes = terminal.first.value;
            const u32 no = terminal.second.value;
            if (yes == no) {
                if (yes != fallthrough_block) JumpTo(yes);
            } else if (no == fallthrough_block) {
                BranchNonZero(cond_reg, yes);
            } else if (yes == fallthrough_block) {
                BranchZero(cond_reg, no);
            } else {
                BranchNonZero(cond_reg, yes);
                JumpTo(no);
            }
            return;
        }
        case TerminalKind::Return: {
            if (terminal.HasValue()) {
                const auto& home = allocation.homes[terminal.value.value];
                if (home.spilled)
                    FrameLoad(a0, home.stack_offset);
                else
                    as.MV(a0, GPR{home.reg});
                Mask(a0, TypeBits(terminal.value_type));
            } else
                as.MV(a0, x0);
            if (lbt_live) EmitExportLBTToPacked(packed_flags);
            if (options.flags_offset != UINT32_MAX)
                Store(packed_flags, state, options.flags_offset, 64);
            Restore();
            if (options.dispatcher && options.exit_handler) {
                HostAddress(t6, options.exit_handler, HostSymbol::Exit);
                as.JR(t6);
            } else
                as.RET();
            return;
        }
        case TerminalKind::Dispatch:
            if (static_pc_known && options.dispatcher && options.exit_handler) {
                StaticExit(static_pc);
                return;
            }
            if (lbt_live) EmitExportLBTToPacked(packed_flags);
            if (options.flags_offset != UINT32_MAX)
                Store(packed_flags, state, options.flags_offset, 64);
            if (options.pc_offset != UINT32_MAX)
                Load(a0, state, options.pc_offset, 64);
            else
                as.MV(a0, x0);
            Restore();
            if (options.dispatcher && options.exit_handler) {
                HostAddress(t6, options.dispatcher, HostSymbol::Dispatcher);
                output.has_indirect_dispatch = true;
                as.JR(t6);
            } else {
                as.MV(a0, x0);
                as.RET();
            }
            return;
        case TerminalKind::Unreachable:
            as.EBREAK();
            return;
        default:
            Reject(CompileError::InvalidProgram);
    }
}

void Writer::FinishResult(OpContext& ctx) {
    if (ctx.saved || ctx.operation.type == Type::Void) return;
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) {
        FrameStore(ctx.destination, home.stack_offset);
        if (ctx.operation.type == Type::V128) FrameStore(ctx.high, home.stack_offset + 8);
    }
}

void Writer::Dispatch(OpContext& ctx) {
    if (ctx.operation.flag_outputs && !FlagProducer(ctx.operation.opcode))
        Reject(CompileError::UnsupportedFlags);
    switch (ctx.operation.opcode) {
#define INST(name, ...)                                                                            \
    case OpCode::name:                                                                             \
        Handle(Op<OpCode::name>{}, ctx);                                                           \
        return;
#include "runtime/ir/opcodes.inc"
#undef INST
        default:
            Reject(CompileError::UnsupportedOpcode);
    }
}

void Writer::Run() {
    const auto has_secondary_entry = [&](u32 idx) {
        const auto& b = source.Blocks()[idx];
        if (!b.node_count || source.Operations()[b.first_node].opcode != OpCode::EntryPoint)
            return false;
        return (*source.Operands(NodeId{b.first_node}).begin()).Payload() != options.entry_pc;
    };
    Prologue();
    if (allocation.block_order.empty() ||
        allocation.block_order.front() != source.EntryBlock().value ||
        has_secondary_entry(allocation.block_order.front())) {
        JumpTo(source.EntryBlock().value);
    }
    for (std::size_t pos = 0; pos < allocation.block_order.size(); ++pos) {
        const auto index = allocation.block_order[pos];
        block = BlockId{index};
        static_pc_known = terminated = false;
        has_fault_location = false;
        const auto& body = source.Blocks()[index];
        if (has_secondary_entry(index)) {
            const auto pc = (*source.Operands(NodeId{body.first_node}).begin()).Payload();
            output.entries.push_back({pc, static_cast<u32>(output.words.size() * 4)});
            lbt_live = false;
            carry_xor = false;
            lbt_dirty_mask = 0;
            Prologue();
        }
        lbt_live = lbt_canonical_blocks;
        carry_xor = false;
        lbt_dirty_mask = lbt_canonical_blocks ? unit_lbt_written_mask : 0u;
        block_offsets[index] = static_cast<u32>(output.words.size() * 4);
        for (u32 n = body.first_node; n < body.first_node + body.node_count; ++n) {
            current = NodeId{n};
            const auto& op = source.Operations()[n];
            const auto& home = allocation.homes[n];
            OpContext ctx{op};
            ctx.node = current;
            ctx.operands = source.Operands(current);
            ctx.width = TypeBits(op.type);
            if (op.type != Type::Void && !home.spilled) {
                ctx.destination = GPR{home.reg};
                if (op.type == Type::V128) ctx.high = GPR{home.high_reg};
            }
            for (const auto operand : ctx.operands) {
                if (ctx.count < ctx.args.size()) ctx.args[ctx.count] = operand;
                ++ctx.count;
            }
            if (op.opcode == OpCode::FaultLocation) {
                guest_pc = ctx.Arg(0).Payload();
                has_fault_location = true;
            }
            Dispatch(ctx);
            FinishResult(ctx);
        }
        const u32 fallthrough =
                (pos + 1 < allocation.block_order.size() &&
                 !has_secondary_entry(allocation.block_order[pos + 1]))
                        ? allocation.block_order[pos + 1]
                        : UINT32_MAX;
        if (!terminated) Terminal(body.terminal, fallthrough);
    }
    for (const auto& jump : jumps) {
        const auto& offsets = jump.local ? local_offsets : block_offsets;
        if (jump.target >= offsets.size() || offsets[jump.target] == UINT32_MAX)
            Reject(CompileError::UnsupportedControlFlow);
        const auto delta = std::int64_t{offsets[jump.target]} - jump.offset;
        if (delta & 3) Reject(CompileError::InvalidProgram);
        Assembler patch{reinterpret_cast<std::uint8_t*>(output.words.data()) + jump.offset, 4};
        if (jump.kind == JumpKind::Unconditional) {
            if (delta < -(1LL << 27) || delta >= (1LL << 27)) Reject(CompileError::CodeTooLarge);
            patch.J(static_cast<int>(delta));
        } else {
            if (delta < -(1LL << 22) || delta >= (1LL << 22)) Reject(CompileError::CodeTooLarge);
            if (jump.kind == JumpKind::Zero)
                patch.BEQZ(jump.reg, static_cast<int>(delta));
            else
                patch.BNEZ(jump.reg, static_cast<int>(delta));
        }
    }
}

void Writer::Handle(Op<OpCode::Goto>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    if (lbt_canonical_blocks) NormalizeLBT();
    BranchNonZero(a0, current.value, true);
}

void Writer::Handle(Op<OpCode::NotGoto>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    if (lbt_canonical_blocks) NormalizeLBT();
    BranchZero(a0, current.value, true);
}

void Writer::Handle(Op<OpCode::BindLabel>, OpContext& ctx) {
    if (lbt_canonical_blocks) {
        if (!terminated) NormalizeLBT();
        lbt_live = true;
        carry_xor = false;
        lbt_dirty_mask = unit_lbt_written_mask;
    }
    local_offsets[ctx.Arg(0).Payload()] = static_cast<u32>(output.words.size() * 4);
    static_pc_known = false;
}
}  // namespace swift::runtime::backend::loongarch64::detail
