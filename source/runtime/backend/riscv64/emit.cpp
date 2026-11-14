#include "abi_layout.inc"
#include "emit.h"
#include "runtime/exit.h"

#include <algorithm>

namespace swift::runtime::backend::riscv64::detail {
namespace {
constexpr u32 kSaveRaOffset = 0;
constexpr u32 kSaveS1Offset = 8;
constexpr u32 kSaveS2BaseOffset = 16;  // s2..s11 (10 * 8 = 80 bytes)
constexpr u32 kSaveFcsrOffset = 96;
constexpr u32 kSaveS0Offset = 104;
constexpr unsigned kFirstSavedSReg = 18;
constexpr unsigned kSavedSRegCount = 10;

// Packed 2-bit table mapping x86 RC (0..3: RNE, RDN, RUP, RTZ) -> RV FRM (0, 2, 3, 1).
constexpr u64 kX86ToRvRoundingMap = 0x78;
constexpr unsigned kMxcsrRoundingShift = 13;
constexpr unsigned kRvFrmShift = 5;
constexpr unsigned kRvFflagsMask = 31;

constexpr std::int64_t kBTypeMin = -4096;
constexpr std::int64_t kBTypeMax = 4094;
constexpr std::int64_t kJTypeMin = -1048576;
constexpr std::int64_t kJTypeMax = 1048574;

constexpr bool IsCanonicalBoolProducer(OpCode code) noexcept {
    switch (code) {
        case OpCode::TestBit:
        case OpCode::TestZero:
        case OpCode::TestNotZero:
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
        case OpCode::FCmpCondSet:
        case OpCode::LocalParitySet:
        case OpCode::TestFlags:
        case OpCode::TestNotFlags:
            return true;
        default:
            return false;
    }
}
}  // namespace

Writer::Writer(const Program& source,
               const Allocation& allocation,
               CompileResult& output,
               CompileOptions options,
               unsigned jump_mode)
        : source(source)
        , allocation(allocation)
        , output(output)
        , options(options)
        , as(output.words)
        , guest_pc(options.entry_pc)
        , jump_mode(jump_mode)
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

void Writer::InstallGuestRounding(GPR scratch0, GPR scratch1) {
    Load(scratch0, s1, options.mxcsr_offset, 32);
    as.SRLI(scratch0, scratch0, kMxcsrRoundingShift);
    as.ANDI(scratch0, scratch0, 3);
    as.LI(scratch1, kX86ToRvRoundingMap);
    as.SLLI(scratch0, scratch0, 1);
    as.SRL(scratch1, scratch1, scratch0);
    as.ANDI(scratch1, scratch1, 3);
    as.SLLI(scratch1, scratch1, kRvFrmShift);
    as.FSCSR(scratch1);
}

void Writer::Prologue() {
    FrameDelta(-std::int64_t{output.frame_bytes});
    if (Linked()) return;
    const auto base = allocation.spill_bytes;
    FrameStore(ra, base + kSaveRaOffset);
    FrameStore(s1, base + kSaveS1Offset);
    for (unsigned i = 0; i != kSavedSRegCount; ++i)
        FrameStore(GPR{kFirstSavedSReg + i}, base + kSaveS2BaseOffset + 8 * i);
    as.FRCSR(t0);
    FrameStore(t0, base + kSaveFcsrOffset);
    FrameStore(s0, base + kSaveS0Offset);
    as.MV(s1, a0);
    if (options.flags_offset != UINT32_MAX)
        Load(s11, s1, options.flags_offset, 64);
    else
        as.MV(s11, x0);
    if (options.mxcsr_offset != UINT32_MAX) InstallGuestRounding(t0, t1);
}

void Writer::Restore() {
    if (!Linked()) {
        const auto base = allocation.spill_bytes;
        FrameLoad(t0, base + kSaveFcsrOffset);
        as.FSCSR(t0);
        FrameLoad(s0, base + kSaveS0Offset);
        for (unsigned i = 0; i != kSavedSRegCount; ++i)
            FrameLoad(GPR{kFirstSavedSReg + i}, base + kSaveS2BaseOffset + 8 * i);
        FrameLoad(s1, base + kSaveS1Offset);
        FrameLoad(ra, base + kSaveRaOffset);
    }
    FrameDelta(output.frame_bytes);
}

void Writer::Exit(u64 reason) {
    if (reason >= ExitValue(NativeExit::UnsupportedAtomic) &&
        reason <= ExitValue(NativeExit::FloatingFault) && options.pc_offset != UINT32_MAX) {
        as.LI(t0, guest_pc);
        Store(t0, s1, options.pc_offset, 64);
    }
    if (!Linked() && options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
    Restore();
    as.LI(a0, reason);
    if (Linked()) {
        HostAddress(t6, options.exit_handler, HostSymbol::Exit);
        as.JR(t6);
    } else
        as.RET();
}

void Writer::StaticExit(u64 pc) {
    // In linked mode s11 stays live across units and is published by SvmNativeExit,
    // SvmNativePending, SvmNativeDispatch miss, CallRegister, and RestoreGuestState.
    Restore();
    if (output.words.size() & 1) as.NOP();
    const auto branch = ByteOffset();
    as.AUIPC(t6, 0);
    as.LD(t5, 16, t6);
    as.FENCE(FenceOrder::R, FenceOrder::RW);
    as.JR(t5);
    const auto data = ByteOffset();
    for (unsigned i = 0; i != 4; ++i) as.GetCodeBuffer().Emit32(0);
    const auto fallback = ByteOffset();
    as.LI(a0, pc);
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
    const auto offset = static_cast<NativeOffset>(ByteOffset() + 16);
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
    if (Linked()) {
        as.LI(t6, SVM_NATIVE_PHASE_HELPER);
        Store(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_PHASE), 64);
    }
    if (options.flags_offset != UINT32_MAX) Store(s11, s1, options.flags_offset, 64);
    if (options.native_fp_environment && Linked()) {
        as.FRCSR(t6);
        as.ANDI(t6, t6, kRvFflagsMask);
        Store(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR), 64);
        Load(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_HOST_FCSR), 64);
        as.FSCSR(t6);
    } else if (!Linked()) {
        FrameLoad(t6, allocation.spill_bytes + kSaveFcsrOffset);
        as.FSCSR(t6);
    }
    as.JALR(ra, 0, target);
    if (options.flags_offset != UINT32_MAX) Load(s11, s1, options.flags_offset, 64);
    if (options.native_fp_environment && Linked()) {
        Load(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FCSR), 64);
        Load(t0, s1, SVM_RV_CTX_REL(SVM_RV_CTX_GUEST_FPSR), 64);
        as.OR(t6, t6, t0);
        as.FSCSR(t6);
    } else if (options.mxcsr_offset != UINT32_MAX) {
        // t0/t6 are caller-saved and do not carry the helper return in a0/a1.
        InstallGuestRounding(t6, t0);
    }
    if (Linked()) {
        as.LI(t6, SVM_NATIVE_PHASE_GUEST);
        Store(t6, s1, SVM_RV_CTX_REL(SVM_RV_CTX_PHASE), 64);
    }
}

bool Writer::HasSecondaryEntry(u32 block_index) const {
    const auto& body = source.Blocks()[block_index];
    if (!body.node_count || source.Operations()[body.first_node].opcode != OpCode::EntryPoint)
        return false;
    const auto pc = (*source.Operands(NodeId{body.first_node}).begin()).Payload();
    return pc != options.entry_pc;
}

void Writer::JumpTo(u32 target, bool local) {
    const auto& offsets = local ? local_offsets : block_offsets;
    if (jump_mode >= 1) {
        if (target < offsets.size() && offsets[target] != UINT32_MAX) {
            const auto delta = std::int64_t{offsets[target]} - std::int64_t{ByteOffset()};
            if (delta < kJTypeMin || delta > kJTypeMax) throw ShortJumpOverflow{};
            as.J(static_cast<std::int32_t>(delta));
            return;
        }
        jumps.push_back({ByteOffset(), target, JumpKind::Jal, 0, local});
        as.NOP();
        return;
    }
    jumps.push_back({ByteOffset(), target, JumpKind::AuipcJalr, 0, local});
    as.AUIPC(t6, 0);
    as.JALR(x0, 0, t6);
}

void Writer::BranchZeroTo(GPR reg, bool nonzero, u32 target, bool local) {
    const auto& offsets = local ? local_offsets : block_offsets;
    if (target < offsets.size() && offsets[target] != UINT32_MAX) {
        const auto delta = std::int64_t{offsets[target]} - std::int64_t{ByteOffset()};
        if (delta >= kBTypeMin && delta <= kBTypeMax) {
            if (nonzero)
                as.BNE(reg, x0, static_cast<std::int32_t>(delta));
            else
                as.BEQ(reg, x0, static_cast<std::int32_t>(delta));
            return;
        }
    } else if (jump_mode >= 2) {
        jumps.push_back({ByteOffset(),
                         target,
                         nonzero ? JumpKind::BranchNeZero : JumpKind::BranchEqZero,
                         static_cast<std::uint8_t>(reg.Index()),
                         local});
        as.NOP();
        return;
    }
    const std::int32_t skip = jump_mode >= 1 ? 8 : 12;
    if (nonzero)
        as.BEQ(reg, x0, skip);
    else
        as.BNE(reg, x0, skip);
    JumpTo(target, local);
}

void Writer::Terminal(const StoredTerminal& terminal, u32 next_block) {
    switch (terminal.kind) {
        case TerminalKind::Jump:
            if (terminal.first.value != next_block) JumpTo(terminal.first.value);
            return;
        case TerminalKind::Branch: {
            const auto& home = allocation.homes[terminal.value.value];
            const auto& def_op = source.Operations()[terminal.value.value];
            const auto view_bits = TypeBits(terminal.value_type);
            GPR cond = a0;
            if (!home.spilled &&
                (IsCanonicalBoolProducer(def_op.opcode) || view_bits >= TypeBits(def_op.type))) {
                cond = GPR{home.reg};
            } else {
                if (home.spilled)
                    FrameLoad(a0, home.stack_offset);
                else
                    as.MV(a0, GPR{home.reg});
                Mask(a0, view_bits);
            }
            if (terminal.first.value == terminal.second.value) {
                if (terminal.first.value != next_block) JumpTo(terminal.first.value);
                return;
            }
            if (terminal.second.value == next_block) {
                BranchZeroTo(cond, true, terminal.first.value);
                return;
            }
            if (terminal.first.value == next_block) {
                BranchZeroTo(cond, false, terminal.second.value);
                return;
            }
            BranchZeroTo(cond, true, terminal.first.value);
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
                if (TypeBits(terminal.value_type) <
                    TypeBits(source.Operations()[terminal.value.value].type)) {
                    Mask(a0, TypeBits(terminal.value_type));
                }
            } else
                as.MV(a0, x0);
            if (!Linked() && options.flags_offset != UINT32_MAX)
                Store(s11, s1, options.flags_offset, 64);
            Restore();
            if (Linked()) {
                HostAddress(t6, options.exit_handler, HostSymbol::Exit);
                as.JR(t6);
            } else
                as.RET();
            return;
        }
        case TerminalKind::Dispatch:
            if (static_pc_known && Linked()) {
                StaticExit(static_pc);
                return;
            }
            if (!Linked() && options.flags_offset != UINT32_MAX)
                Store(s11, s1, options.flags_offset, 64);
            if (options.pc_offset != UINT32_MAX)
                Load(a0, s1, options.pc_offset, 64);
            else
                as.MV(a0, x0);
            Restore();
            if (Linked()) {
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

void Writer::PatchJumps() {
    for (const auto& jump : jumps) {
        const auto& offsets = jump.local ? local_offsets : block_offsets;
        if (jump.target >= offsets.size() || offsets[jump.target] == UINT32_MAX)
            Reject(CompileError::UnsupportedControlFlow);
        const auto delta = std::int64_t{offsets[jump.target]} - std::int64_t{jump.offset};
        auto* site = reinterpret_cast<std::uint8_t*>(output.words.data()) + jump.offset;
        switch (jump.kind) {
            case JumpKind::BranchEqZero:
            case JumpKind::BranchNeZero: {
                if (delta < kBTypeMin || delta > kBTypeMax) throw ShortJumpOverflow{};
                Assembler patch{site, 4};
                if (jump.kind == JumpKind::BranchNeZero)
                    patch.BNE(GPR{jump.reg}, x0, static_cast<std::int32_t>(delta));
                else
                    patch.BEQ(GPR{jump.reg}, x0, static_cast<std::int32_t>(delta));
                break;
            }
            case JumpKind::Jal: {
                if (delta < kJTypeMin || delta > kJTypeMax) throw ShortJumpOverflow{};
                Assembler patch{site, 4};
                patch.J(static_cast<std::int32_t>(delta));
                break;
            }
            case JumpKind::AuipcJalr: {
                if (delta < INT32_MIN || delta > INT32_MAX - 2048)
                    Reject(CompileError::CodeTooLarge);
                const auto high = (delta + 0x800) >> 12;
                const auto low = delta - (high << 12);
                Assembler patch{site, 8};
                patch.AUIPC(t6, static_cast<u32>(high) & 0xfffff);
                patch.JALR(x0, static_cast<int>(low), t6);
                break;
            }
        }
    }
}

void Writer::Run() {
    Prologue();
    const u32 entry = source.EntryBlock().value;
    if (allocation.block_order.empty() || allocation.block_order.front() != entry ||
        HasSecondaryEntry(entry)) {
        JumpTo(entry);
    }
    for (std::size_t order_idx = 0; order_idx < allocation.block_order.size(); ++order_idx) {
        const auto index = allocation.block_order[order_idx];
        block = BlockId{index};
        static_pc_known = terminated = false;
        has_fault_location = false;
        const auto& body = source.Blocks()[index];
        if (HasSecondaryEntry(index)) {
            const auto pc = (*source.Operands(NodeId{body.first_node}).begin()).Payload();
            output.entries.push_back({pc, ByteOffset()});
            Prologue();
        }
        block_offsets[index] = ByteOffset();
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
        const bool flush_after_block = NeedColdStubFlush(false);
        u32 next_block = UINT32_MAX;
        if (!flush_after_block && order_idx + 1 < allocation.block_order.size()) {
            const u32 candidate = allocation.block_order[order_idx + 1];
            if (!HasSecondaryEntry(candidate)) next_block = candidate;
        }
        if (!terminated) Terminal(body.terminal, next_block);
        if (flush_after_block) FlushColdStubs(false);
    }
    FlushColdStubs(false);
    PatchJumps();
}

void Writer::Handle(Op<OpCode::Goto>, OpContext& ctx) {
    const GPR cond = ReadRef(ctx.Arg(0), a0);
    if (ctx.destination != cond) as.MV(ctx.destination, cond);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    BranchZeroTo(cond, true, current.value, true);
}

void Writer::Handle(Op<OpCode::NotGoto>, OpContext& ctx) {
    const GPR cond = ReadRef(ctx.Arg(0), a0);
    if (ctx.destination != cond) as.MV(ctx.destination, cond);
    Mask(ctx.destination, ctx.width);
    const auto& home = allocation.homes[ctx.node.value];
    if (home.spilled) FrameStore(ctx.destination, home.stack_offset);
    ctx.saved = true;
    BranchZeroTo(cond, false, current.value, true);
}

void Writer::Handle(Op<OpCode::BindLabel>, OpContext& ctx) {
    local_offsets[ctx.Arg(0).Payload()] = ByteOffset();
    static_pc_known = false;
}
}  // namespace swift::runtime::backend::riscv64::detail
