#include "emit.h"

#define __ masm.exact.

namespace swift::runtime::backend::arm64::detail {

void Writer::TailJump(HostSymbol symbol,
                      std::uintptr_t destination,
                      bool link,
                      u64 target_pc,
                      u32 conditional_head) {
    const bool is_dispatcher = options.relocatable ? symbol == HostSymbol::Dispatcher
                                                   : destination == options.dispatcher;
    const bool is_exit =
            options.relocatable ? symbol == HostSymbol::Exit : destination == options.exit_handler;
    const u32 shared_literal = is_dispatcher ? dispatcher_literal
                               : is_exit     ? exit_literal
                                             : UINT32_MAX;
    if (!link && shared_literal != UINT32_MAX) {
        const auto distance = static_cast<std::int64_t>(shared_literal) -
                              static_cast<std::int64_t>(output.words.size());
        if (distance >= -(INT64_C(1) << 18) && distance < (INT64_C(1) << 18)) {
            __ ldr(v::x16, distance);
            __ br(v::x16);  // LDR x16,immutable dispatcher/exit DATA; BR x16.
            return;
        }
    }
    // CodeArena aligns entries. Keep DATA aligned without executing padding
    // before a resolved branch; the BR also skips padding on the far path.
    // A conditional head reaches this cold gate only when unresolved/far.
    // Its zero path already transfers elsewhere, so alignment here is cold.
    if (conditional_head != UINT32_MAX && (output.words.size() & 1)) __ nop();
    const auto branch = static_cast<u32>(output.words.size());
    const auto padding = branch & 1u;
    const auto pointer = branch + 2 + padding;
    __ ldr(v::x16, static_cast<std::int64_t>(2 + padding));
    __ br(v::x16);
    if (padding) __ nop();
    masm.Data64(destination);
    if (options.relocatable && !link)
        RecordAddress(pointer * 4, RelocationKind::Absolute64, symbol);
    if (!link && is_dispatcher) dispatcher_literal = pointer;
    if (!link && is_exit) exit_literal = pointer;
    if (link) {
        output.links.push_back({LinkKind::Static,
                                pointer * 4,
                                (conditional_head == UINT32_MAX ? branch : conditional_head) * 4,
                                (pointer + 2) * 4,
                                target_pc});
        if (conditional_head != UINT32_MAX) {
            masm.Patch(conditional_head, branch);
        }
        // Only this self link can carry the local TEST tag. Unlinking must
        // complete its actual next value before the ordinary dispatcher.
        if (source.ScopeKind() == Scope::Block && deferred_test.bits == 64 &&
            target_pc == options.entry_pc)
            CompleteDeferredTest();
        // This edge is already outside the source unit's spill frame.
        // Resolved B26/far links bypass it; only cold dispatch needs PC.
        Immediate(0, target_pc);
        PCMemory(false);
        TailJump(symbol, destination);  // The cold dispatcher address owns its relocation.
    }
}

// x19 addresses embedded state. Prefix metadata uses unscaled displacements;
// the thread-L1 field in the tail uses a positive scaled displacement.
void Writer::ContextMemory(bool load, unsigned value, int offset) {
    const MemOperand field(v::x19, offset);
    if (offset >= 0) {
        if (load)
            __ ldr(X(value), field);
        else
            __ str(X(value), field);
    } else if (load)
        __ ldur(X(value), field);
    else
        __ stur(X(value), field);
}

bool Writer::InterruptibleDispatch() const {
    if (!Linked() || !options.fault_locations || options.dispatch_mode == DispatchMode::Callback)
        return false;
    const auto& body = source.Blocks()[block.value];
    if (body.terminal.kind != TerminalKind::Dispatch || !body.node_count) return false;
    const NodeId tail{body.first_node + body.node_count - 1};
    const auto& operation = source.Operations()[tail.value];
    if (operation.opcode != OpCode::StoreUniform || operation.Metadata() != OperationMetadata{})
        return false;
    auto operands = source.Operands(tail).begin();
    const auto uniform = *operands++, value = *operands;
    if (uniform.TypeView() != Type::U64 || uniform.Payload() != options.pc_offset ||
        value.Kind() != OperandKind::Value || Width(View(value)) != 64)
        return false;
    u64 ignored;
    if (ConstantPC(value, ignored)) return false;
    const auto& producer = source.Operations()[value.Payload()];
    if (producer.opcode == OpCode::Select && producer.block == block && Bits(producer.type) == 64) {
        auto choices = source.Operands(NodeId{static_cast<u32>(value.Payload())}).begin();
        ++choices;
        const auto yes = *choices++, no = *choices;
        if (ConstantPC(yes, ignored) && ConstantPC(no, ignored)) return false;
    }
    // This final store retires the frame and dispatches x0 through L1. Inspect
    // only its direct producers, matching ObservePCStore's static-link cases.
    return true;
}

void Writer::DynamicDispatch() {
    output.has_indirect_dispatch = true;
    if (options.dispatch_mode == DispatchMode::Callback) {
        TailJump(HostSymbol::Dispatcher, options.dispatcher);
        return;
    }
    // The source frame is retired. Each site retains its own indirect branch;
    // the selected slot in x7 also serves the shared L2 ready-only refill.
    const auto count = [&](int offset) {
        if (options.dispatch_mode != DispatchMode::InlineCounted) return;
        ContextMemory(true, 4, offset);
        __ add(v::x4, v::x4, Operand(1));
        ContextMemory(false, 4, offset);
    };
    count(abi::ContextDispatchesOffset);
    ContextMemory(true, 16, abi::ContextL1Offset);
    __ ubfiz(v::x7, v::x0, abi::L1EntryShift, abi::L1Bits);
    __ add(v::x7, v::x16, Operand(v::x7));
    const auto probe = static_cast<u32>(output.words.size()) * 4;
    __ ldp(v::x16, v::x17, MemOperand(v::x7));
    if (options.fault_locations) {
        output.faults.push_back({probe, probe + 4});
        output.faults.back().SetKind(FaultSiteKind::DispatchInterrupt);
    }
    __ sub(v::x16, v::x16, Operand(v::x0));  // Preserve guest NZCV.
    const auto miss = static_cast<u32>(output.words.size());
    __ cbnz(v::x16, std::int64_t{0});
    count(abi::ContextHitsOffset);
    __ br(v::x17);
    masm.Patch(miss, masm.Here());
    TailJump(HostSymbol::DispatcherL2, options.l2_dispatcher);
}

void Writer::PublishPCAndJump(u64 target_pc) {
    if (!options.deferred_pc) {
        Immediate(0, target_pc);
        PCMemory(false);
    }
    TailJump(HostSymbol::Dispatcher, options.dispatcher, true, target_pc);
}

}  // namespace swift::runtime::backend::arm64::detail

#undef __
