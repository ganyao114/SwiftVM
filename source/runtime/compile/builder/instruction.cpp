#include "ir_builder.h"

#include <exception>

namespace swift::runtime::compile {
IrBuilder::GuestInstructionScope::GuestInstructionScope(IrBuilder& input)
        : builder(input), exceptions(std::uncaught_exceptions()) {
    builder.BeginGuestInstruction();
}

IrBuilder::GuestInstructionScope::~GuestInstructionScope() {
    if (std::uncaught_exceptions() > exceptions) builder.CancelGuestInstruction();
    else builder.CommitGuestInstruction();
}

void IrBuilder::BeginGuestInstruction() {
    ASSERT(!guest_instruction);
    ASSERT(pending_values.empty());
    guest_instruction = true;
}

void IrBuilder::CancelGuestInstruction() {
    pending_values.clear();
    guest_instruction = false;
    PruneImmediateSources();
}

void IrBuilder::CommitGuestInstruction() {
    if (!guest_instruction) return;

    // Explicit-type producers have their result contract at construction.
    // Unknown flags stay conservative even when no pseudo was appended here;
    // a lack of current uses does not promise an absence of later outputs.
    for (const auto& pending : pending_values) {
        FoldImmediate(pending.inst, pending.flags, pending.position);
        FoldValue(pending.inst, pending.flags, pending.position);
    }
    CancelGuestInstruction();
}

void IrBuilder::ObserveFlagOutput(const ir::Inst& inst) const {
    const auto op = inst.GetOp();
    if (op != ir::OpCode::SaveFlags && op != ir::OpCode::GetFlags &&
        op != ir::OpCode::BranchOnlyFlags) return;
    const auto* producer = inst.GetArg<ir::Value>(0).Def();
    for (const auto& pending : pending_values) {
        if (pending.inst != producer) continue;
        ASSERT_MSG(pending.flags.Allows(inst.GetArg<ir::Flags>(1)),
                   "flags output exceeds the producer's construction contract");
        return;
    }
}

}  // namespace swift::runtime::compile
