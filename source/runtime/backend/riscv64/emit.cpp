#include "abi_layout.inc"
#include "emit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
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
        , local_offsets(source.Operations().size(), UINT32_MAX) {}

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
    if (options.dispatcher && options.exit_handler) return;
    const auto base = allocation.spill_bytes;
    FrameStore(ra, base);
    FrameStore(s1, base + 8);
    for (unsigned i = 0; i != 10; ++i) FrameStore(GPR{18 + i}, base + 16 + 8 * i);
    as.FRCSR(t0);
    FrameStore(t0, base + 96);
    as.MV(s1, a0);
    if (options.flags_offset != UINT32_MAX)
        Load(s11, s1, options.flags_offset, 64);
    else
        as.MV(s11, x0);
    if (options.mxcsr_offset != UINT32_MAX) {
        Load(t0, s1, options.mxcsr_offset, 32);
        as.SRLI(t0, t0, 13);
        as.ANDI(t0, t0, 3);
        // x86 nearest/down/up/zero -> RV RNE/RDN/RUP/RTZ.
        as.LI(t1, 0x78);
        as.SLLI(t0, t0, 1);
        as.SRL(t1, t1, t0);
        as.ANDI(t1, t1, 3);
        as.SLLI(t1, t1, 5);
        as.FSCSR(t1);
    }
}

void Writer::Restore() {
    if (!(options.dispatcher && options.exit_handler)) {
        const auto base = allocation.spill_bytes;
        FrameLoad(t0, base + 96);
        as.FSCSR(t0);
        for (unsigned i = 0; i != 10; ++i) FrameLoad(GPR{18 + i}, base + 16 + 8 * i);
        FrameLoad(s1, base + 8);
        FrameLoad(ra, base);
    }
    FrameDelta(output.frame_bytes);
}

void Writer::Exit(u64 reason) {
    if (reason >= 6 && options.pc_offset != UINT32_MAX) {
        as.LI(t0, guest_pc);
        Store(t0, s1, options.pc_offset, 64);
    }
    if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
    Restore();
    as.LI(a0, reason);
    if (options.dispatcher && options.exit_handler) {
        HostAddress(t6, options.exit_handler, HostSymbol::Exit);
        as.JR(t6);
    } else
        as.RET();
}

void Writer::StaticExit(u64 pc) {
    if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
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
    if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
    if (options.native_fp_environment && options.dispatcher && options.exit_handler) {
        as.FRCSR(t6);
        as.ANDI(t6, t6, 31);
        Store(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR), 64);
        Load(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_HOST_FCSR), 64);
        as.FSCSR(t6);
    } else if (!(options.dispatcher && options.exit_handler)) {
        FrameLoad(t6, allocation.spill_bytes + 96);
        as.FSCSR(t6);
    }
    as.JALR(ra, 0, target);
    if (options.flags_offset != UINT32_MAX) Load(s11, s1, options.flags_offset, 64);
    if (options.native_fp_environment && options.dispatcher && options.exit_handler) {
        Load(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FCSR), 64);
        Load(t0, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR), 64);
        as.OR(t6, t6, t0);
        as.FSCSR(t6);
    } else if (options.mxcsr_offset != UINT32_MAX) {
        Load(t6, s1, options.mxcsr_offset, 32);
        as.SRLI(t6, t6, 13);
        as.ANDI(t6, t6, 3);
        // t0 is caller-saved and does not carry the helper return.
        as.LI(t0, 0x78);
        as.SLLI(t6, t6, 1);
        as.SRL(t0, t0, t6);
        as.ANDI(t0, t0, 3);
        as.SLLI(t0, t0, 5);
        as.FSCSR(t0);
    }
}

void Writer::JumpTo(u32 target, bool local) {
    jumps.push_back({static_cast<u32>(output.words.size() * 4), target, local});
    as.AUIPC(t6, 0);
    as.JALR(x0, 0, t6);
}

void Writer::Terminal(const StoredTerminal& terminal) {
    switch (terminal.kind) {
        case TerminalKind::Jump:
            JumpTo(terminal.first.value);
            return;
        case TerminalKind::Branch: {
            const auto& home = allocation.homes[terminal.value.value];
            if (home.spilled)
                FrameLoad(a0, home.stack_offset);
            else
                as.MV(a0, GPR{home.reg});
            Mask(a0, TypeBits(terminal.value_type));
            as.BEQ(a0, x0, 12);
            JumpTo(terminal.first.value);
            JumpTo(terminal.second.value);
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
            if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
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
            if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
            if (options.pc_offset != UINT32_MAX)
                Load(a0, s1, options.pc_offset, 64);
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
    Prologue();
    JumpTo(source.EntryBlock().value);
    for (const auto index : allocation.block_order) {
        block = BlockId{index};
        static_pc_known = terminated = false;
        const auto& body = source.Blocks()[index];
        if (body.node_count && source.Operations()[body.first_node].opcode == OpCode::EntryPoint) {
            const auto pc = (*source.Operands(NodeId{body.first_node}).begin()).Payload();
            if (pc != options.entry_pc) {
                output.entries.push_back({pc, static_cast<u32>(output.words.size() * 4)});
                Prologue();
                JumpTo(index);
            }
        }
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
            if (op.opcode == OpCode::FaultLocation) guest_pc = ctx.Arg(0).Payload();
            const auto first = static_cast<u32>(output.words.size() * 4);
            Dispatch(ctx);
            FinishResult(ctx);
            const auto last = static_cast<u32>(output.words.size() * 4);
            if (options.fault_locations && HasAnyEffect(EffectsFor(op.opcode), Effect::MayFault) &&
                first != last)
                output.faults.push_back({first, last, guest_pc});
        }
        if (!terminated) Terminal(body.terminal);
    }
    for (const auto& jump : jumps) {
        const auto& offsets = jump.local ? local_offsets : block_offsets;
        if (jump.target >= offsets.size() || offsets[jump.target] == UINT32_MAX)
            Reject(CompileError::UnsupportedControlFlow);
        const auto delta = std::int64_t{offsets[jump.target]} - jump.offset;
        if (delta < INT32_MIN || delta > INT32_MAX - 2048) Reject(CompileError::CodeTooLarge);
        const auto high = (delta + 0x800) >> 12;
        const auto low = delta - (high << 12);
        Assembler patch{reinterpret_cast<std::uint8_t*>(output.words.data()) + jump.offset, 8};
        patch.AUIPC(t6, static_cast<u32>(high) & 0xfffff);
        patch.JALR(x0, static_cast<int>(low), t6);
    }
}

void Writer::Handle(Op<OpCode::Goto>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    as.BEQ(a0, x0, 12);
    JumpTo(current.value, true);
}

void Writer::Handle(Op<OpCode::NotGoto>, OpContext& ctx) {
    Read(ctx.Arg(0), a0);
    as.MV(ctx.destination, a0);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    as.BNE(a0, x0, 12);
    JumpTo(current.value, true);
}

void Writer::Handle(Op<OpCode::BindLabel>, OpContext& ctx) {
    local_offsets[ctx.Arg(0).Payload()] = static_cast<u32>(output.words.size() * 4);
    static_pc_known = false;
}
}  // namespace swift::runtime::backend::riscv64::detail
