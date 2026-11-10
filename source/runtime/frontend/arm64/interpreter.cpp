#include <array>
#include <cstring>
#include "interpreter.h"
#include "semantics.h"
#include "sink.h"

namespace swift::arm64 {
namespace {
struct FetchedInstruction {
    std::array<std::uint8_t, 4> bytes{};
    DecodeOneResult decoded;
    StepResult failure;
};

FetchedInstruction FetchInstruction(std::uint64_t pc,
                                    const DirectServices& services,
                                    DecodeTiming* timing) {
    FetchedInstruction out;
    out.failure.pc = out.failure.next_pc = pc;
    out.decoded.error = DecodeError::InvalidInput;
    if ((pc & 3) || pc > UINT64_MAX - 4) {
        out.failure.kind = StepKind::InvalidEncoding;
        return out;
    }
    if (!services.fetch) {
        out.failure.kind = StepKind::Unsupported;
        return out;
    }
    const auto fetched = services.fetch(services.opaque, pc, out.bytes);
    if (fetched.bytes < 4) {
        out.failure.kind = fetched.access.status == AccessStatus::Fault ? StepKind::Fault
                           : fetched.access.status == AccessStatus::UnsupportedBeforeAccess
                                   ? StepKind::Unsupported
                                   : StepKind::Truncated;
        out.failure.fault_address = fetched.access.fault_address;
        return out;
    }
    out.decoded = DecodeOne(out.bytes, pc, timing);
    if (!out.decoded) out.failure.kind = StepKind::InvalidEncoding;
    return out;
}
}  // namespace

StepResult InterpretDecoded(const Instruction& instruction,
                            std::span<const std::uint8_t> bytes,
                            runtime::arm64::GuestState& state,
                            const DirectServices& services) {
    const auto pc = state.pc;
    StepResult out;
    out.pc = out.next_pc = pc;
    runtime::ir::direct::ResetUnsupportedOperation();
    if (instruction.PC() != pc || (pc & 3) || bytes.size() < 4) {
        out.kind = bytes.size() < 4 ? StepKind::Truncated : StepKind::InvalidEncoding;
        return out;
    }
    const auto word = instruction.Word();
    if (std::memcmp(bytes.data(), &word, 4)) {
        out.kind = StepKind::InvalidEncoding;
        return out;
    }
    detail::DirectSink sink{state, services};
    runtime::ir::TargetInfo event_target;
    if (services.guest_events)
        event_target.features = static_cast<std::uint64_t>(runtime::ir::TargetFeature::GuestEvents);
    detail::Semantics<detail::DirectSink> semantics{sink, {}, event_target};
    try {
        semantics.Execute(instruction);
        using K = typename decltype(semantics)::Control::Kind;
        const auto& control = semantics.control;
        if (control.kind == K::Jump)
            state.pc = control.yes;
        else if (control.kind == K::Conditional)
            state.pc = control.condition.raw.number.bits ? control.yes : control.no;
        else if (control.kind == K::Dispatch)
            state.pc = control.destination.IsImm()
                               ? control.destination.imm.bits
                               : std::uint64_t(control.destination.value.raw.number.bits);
        else if (control.kind == K::Service) {
            state.pc = control.yes;
            out.kind = StepKind::Service;
            out.service = control.service;
        } else
            state.pc = pc + 4;
        out.next_pc = state.pc;
        out.instructions = 1;
        out.decoded_bytes = 4;
        out.code_changed = sink.code_changed;
    } catch (const detail::Rejected& e) {
        out.kind = e.error == BuildError::InvalidEncoding ? StepKind::InvalidEncoding
                   : sink.observed_effects                ? StepKind::Incomplete
                                                          : StepKind::Unsupported;
    } catch (const runtime::ir::direct::Exit& e) {
        if (e.kind == runtime::ir::direct::ExitKind::Fault) state.reservation_valid = 0;
        out.kind = e.kind == runtime::ir::direct::ExitKind::Fault     ? StepKind::Fault
                   : e.kind == runtime::ir::direct::ExitKind::Service ? StepKind::Service
                   : sink.observed_effects                            ? StepKind::Incomplete
                                                                      : StepKind::Unsupported;
        out.fault_address = e.address;
        out.service = e.service;
        out.code_changed = sink.code_changed;
    }
    return out;
}

StepResult InterpretOne(runtime::arm64::GuestState& state,
                        const DirectServices& services,
                        DecodeTiming* timing) {
    const auto fetched = FetchInstruction(state.pc, services, timing);
    if (!fetched.decoded) return fetched.failure;
    return InterpretDecoded(fetched.decoded.instruction, fetched.bytes, state, services);
}

StepResult InterpretBlock(runtime::arm64::GuestState& state,
                          const DirectServices& services,
                          std::size_t budget,
                          DecodeTiming* timing,
                          ReadyEntryLookup ready) {
    StepResult out;
    out.pc = out.next_pc = state.pc;
    if (!budget) {
        out.kind = StepKind::Unsupported;
        return out;
    }
    for (std::size_t i = 0; i < budget; ++i) {
        if (i && ready.Contains(state.pc)) break;
        const auto fetched = FetchInstruction(state.pc, services, timing);
        const auto step =
                fetched.decoded
                        ? InterpretDecoded(
                                  fetched.decoded.instruction, fetched.bytes, state, services)
                        : fetched.failure;
        out.pc = step.pc;
        out.next_pc = step.next_pc;
        out.instructions += step.instructions;
        out.decoded_bytes += step.decoded_bytes;
        out.code_changed |= step.code_changed;
        if (step.kind != StepKind::Completed) {
            out.kind = step.kind;
            out.service = step.service;
            out.fault_address = step.fault_address;
            break;
        }
        if (step.code_changed || fetched.decoded.instruction.Boundary()) break;
    }
    return out;
}
}  // namespace swift::arm64
