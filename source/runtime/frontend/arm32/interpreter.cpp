#include <array>
#include "interpreter.h"
#include "sink.h"

namespace swift::arm32 {
namespace {
struct FetchedInstruction {
    std::array<u8, 4> bytes{};
    DecodeOneResult decoded{DecodeError::InvalidInput};
    StepResult failure;
};

StepKind FetchFailure(const CodeReadResult& result) noexcept {
    return result.access.status == AccessStatus::Fault                     ? StepKind::Fault
           : result.access.status == AccessStatus::UnsupportedBeforeAccess ? StepKind::Unsupported
                                                                           : StepKind::Truncated;
}

FetchedInstruction FetchInstruction(u64 pc, const DirectServices& services, DecodeTiming* timing) {
    FetchedInstruction out;
    out.failure.pc = out.failure.next_pc = pc;
    const auto location = Location{pc};
    if (location.Address() & (location.Thumb() ? 1u : 3u)) {
        out.failure.kind = StepKind::InvalidEncoding;
        return out;
    }
    if (!services.fetch) {
        out.failure.kind = StepKind::Unsupported;
        return out;
    }
    const std::size_t first_size = location.Thumb() ? 2 : 4;
    const auto first =
            services.fetch(services.opaque, location.Address(), {out.bytes.data(), first_size});
    if (first.bytes < first_size) {
        out.failure.kind = FetchFailure(first);
        out.failure.fault_address = first.access.fault_address;
        return out;
    }
    std::size_t length = first_size;
    if (location.Thumb() && IsThumb32(u16(out.bytes[0]) | (u16(out.bytes[1]) << 8))) {
        const auto second = services.fetch(
                services.opaque, u32(location.Address() + 2), {out.bytes.data() + 2, 2});
        if (second.bytes < 2) {
            out.failure.kind = FetchFailure(second);
            out.failure.fault_address = second.access.fault_address;
            return out;
        }
        length = 4;
    }
    out.decoded = DecodeOne({out.bytes.data(), length}, pc, timing);
    if (!out.decoded)
        out.failure.kind = out.decoded.error == DecodeError::Truncated ? StepKind::Truncated
                                                                       : StepKind::InvalidEncoding;
    return out;
}

bool Matches(const Instruction& instruction, std::span<const u8> bytes) noexcept {
    const auto first = u16(bytes[0]) | (u16(bytes[1]) << 8);
    u32 word = first;
    if (instruction.Length() == 4) {
        const auto second = u16(bytes[2]) | (u16(bytes[3]) << 8);
        word = instruction.Thumb() ? (u32(first) << 16) | second : first | (u32(second) << 16);
    }
    return word == instruction.Word();
}
}  // namespace

StepResult InterpretDecoded(const Instruction& instruction,
                            std::span<const u8> bytes,
                            runtime::arm32::GuestState& state,
                            const DirectServices& services,
                            Profile profile) {
    const auto pc = state.pc;
    StepResult out;
    out.pc = out.next_pc = pc;
    if (instruction.PC() != pc || bytes.size() < instruction.Length() ||
        (instruction.Length() != 2 && instruction.Length() != 4)) {
        out.kind = bytes.size() < instruction.Length() ? StepKind::Truncated
                                                       : StepKind::InvalidEncoding;
        return out;
    }
    if (!Matches(instruction, bytes)) {
        out.kind = StepKind::InvalidEncoding;
        return out;
    }
    detail::DirectSink sink{state, services};
    runtime::ir::TargetInfo target;
    if (services.guest_events)
        target.features = static_cast<u64>(runtime::ir::TargetFeature::GuestEvents);
    detail::Semantics<detail::DirectSink> semantics{sink, {}, target, profile};
    try {
        semantics.Execute(instruction);
        using Kind = decltype(semantics)::Control::Kind;
        const auto& control = semantics.control;
        switch (control.kind) {
            case Kind::None:
                state.pc = instruction.NextPC();
                break;
            case Kind::Jump:
                state.pc = control.yes;
                break;
            case Kind::Conditional:
                state.pc = sink.Truthy(control.condition.raw) ? control.yes : control.no;
                break;
            case Kind::Dispatch:
                state.pc = control.destination.IsImm()
                                   ? control.destination.imm.bits
                                   : static_cast<u64>(control.destination.value.raw.number.bits);
                break;
            case Kind::Service:
                state.pc = control.yes;
                out.kind = StepKind::Service;
                out.service = control.service;
                break;
        }
        out.next_pc = state.pc;
        out.instructions = 1;
        out.decoded_bytes = instruction.Length();
        out.code_changed = sink.code_changed;
    } catch (const detail::Rejected& failure) {
        out.kind = failure.error == BuildError::InvalidEncoding ? StepKind::InvalidEncoding
                   : sink.observed_effects                      ? StepKind::Incomplete
                                                                : StepKind::Unsupported;
    } catch (const runtime::ir::direct::Exit& failure) {
        using ExitKind = runtime::ir::direct::ExitKind;
        if (failure.kind == ExitKind::Fault) state.reservation_valid = 0;
        out.kind = failure.kind == ExitKind::Fault     ? StepKind::Fault
                   : failure.kind == ExitKind::Service ? StepKind::Service
                   : sink.observed_effects             ? StepKind::Incomplete
                                                       : StepKind::Unsupported;
        out.fault_address = failure.address;
        out.service = failure.service;
        out.code_changed = sink.code_changed;
    }
    return out;
}

StepResult InterpretOne(runtime::arm32::GuestState& state,
                        const DirectServices& services,
                        DecodeTiming* timing,
                        Profile profile) {
    const auto fetched = FetchInstruction(state.pc, services, timing);
    if (!fetched.decoded) return fetched.failure;
    return InterpretDecoded(fetched.decoded.instruction, fetched.bytes, state, services, profile);
}

StepResult InterpretBlock(runtime::arm32::GuestState& state,
                          const DirectServices& services,
                          std::size_t budget,
                          DecodeTiming* timing,
                          ReadyEntryLookup ready,
                          Profile profile) {
    StepResult out;
    out.pc = out.next_pc = state.pc;
    if (!budget) {
        out.kind = StepKind::Unsupported;
        return out;
    }
    for (std::size_t i = 0; i < budget; ++i) {
        if (i && ready.Contains(state.pc)) break;
        const auto fetched = FetchInstruction(state.pc, services, timing);
        const auto step = fetched.decoded ? InterpretDecoded(fetched.decoded.instruction,
                                                             fetched.bytes,
                                                             state,
                                                             services,
                                                             profile)
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
}  // namespace swift::arm32
