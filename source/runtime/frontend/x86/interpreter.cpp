#include "interpreter.h"

#include <algorithm>
#include <array>
#include "runtime/arch/x86/state.h"
#include "runtime/ir/direct/interpreter.h"
#include "semantics.h"

namespace swift::x86 {
namespace {
using namespace runtime::ir;
namespace arch = runtime::x86;
namespace direct = runtime::ir::direct;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// The x86 adapter only binds architectural fields and structured control.
// Every numerical operation executes through the reusable IR direct consumer.
class DirectSink final : public direct::Interpreter {
    using u64 = std::uint64_t;

public:
    DirectSink(arch::GuestState& state, const DirectServices& services)
            : Interpreter({{reinterpret_cast<std::uint8_t*>(&state), sizeof state},
                           state.packed_flags,
                           state.pc,
                           state.mxcsr},
                          {services.opaque,
                           services.read,
                           services.write,
                           services.atomic,
                           services.cpuid})
            , state(state)
            , services(services) {}

    void PublishRegisters() {}

    void PublishGPRs(std::uint16_t = UINT16_MAX) {}

    template <class Body> void IfNonZero(Value value, Body&& body) {
        if (value.number.bits != 0) body();
    }

    void FinishDispatch(u64 service) { exit_service = service; }

    void StateMemory(unsigned kind, Value address, Value requested, u64, u64 next_pc) {
        if (!services.state_image) throw detail::Rejected{BuildError::Unsupported};
        ObserveAccess(services.state_image(services.opaque,
                                           kind,
                                           static_cast<u64>(address.number.bits),
                                           static_cast<u64>(requested.number.bits)));
        state.pc = next_pc;
        if ((kind & 255) == 1 || (kind & 255) == 3) exit_service = 3;
    }

    Value ReadGpr(u32 offset, unsigned, unsigned bits) {
        return LoadUniform(ScalarType(bits), {Operand::Uniform(offset, ScalarType(bits))}).value;
    }

    void WriteGpr(u32 offset, unsigned, unsigned bits, Value value) {
        StoreUniform(Type::Void,
                     {Operand::Uniform(offset, bits == 32 ? Type::U64 : ScalarType(bits)),
                      Operand::ValueOf(value, ScalarType(bits))});
    }

    Value ReadAddressGpr(unsigned) { return {}; }

    Value ReadXmm(u32 offset, unsigned) {
        return LoadUniform(Type::V128, {Operand::Uniform(offset, Type::V128)}).value;
    }

    void WriteXmm(u32 offset, unsigned, Value value) {
        StoreUniform(Type::Void,
                     {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value, Type::V128)});
    }

    void InvalidateXmm(unsigned) {}

    Value X87Zero() { return LoadImm(Type::U64, {Operand::Immediate(0)}).GetValue(); }

    Value X87Address(u32 offset) {
        return GetUniformAddress(Type::U64, {Operand::Immediate(offset, Type::U32)}).GetValue();
    }

    std::pair<arch::X87Command, unsigned> X87Window(arch::X87Command command,
                                                    unsigned,
                                                    const InstructionData&,
                                                    InstructionRange,
                                                    std::span<const std::uint8_t>) const {
        return {command, 1};
    }

    static Type ScalarType(unsigned bits) {
        return bits == 8 ? Type::U8 : bits == 16 ? Type::U16 : bits == 32 ? Type::U32 : Type::U64;
    }

    Value AddressExpression(Type type, std::initializer_list<Operand> operands) {
        return GetOperand(type, operands).GetValue();
    }

    Value ShiftedAddress(TargetInfo, Type, Value, Value, std::uint8_t) { return {}; }

    void PrepareFault(StateLayout, u64 pc, TargetInfo, bool) { state.pc = pc; }

    u64 exit_service{};

private:
    arch::GuestState& state;
    const DirectServices& services;
};

struct FetchedInstruction {
    std::array<std::uint8_t, 15> bytes{};
    std::size_t length{};
    DecodeOneResult decoded{DecodeError::InvalidInput, {}};
    StepResult failure;

    // Form the view on the final owner, including after a by-value return.
    std::span<const std::uint8_t> Source() const {
        return std::span<const std::uint8_t>{bytes}.first(length);
    }
};

FetchedInstruction FetchInstruction(u64 pc, const DirectServices& services, DecodeTiming* timing) {
    FetchedInstruction out;
    out.failure.pc = out.failure.next_pc = pc;
    if (!services.fetch) {
        out.failure.kind = StepKind::Unsupported;
        return out;
    }
    const auto fetched = services.fetch(services.opaque, pc, out.bytes);
    if (fetched.access.status != AccessStatus::Completed && !fetched.bytes) {
        out.failure.kind = fetched.access.status == AccessStatus::Fault ? StepKind::Fault
                                                                        : StepKind::Unsupported;
        out.failure.fault_address = fetched.access.fault_address;
        return out;
    }
    out.length = std::min(fetched.bytes, out.bytes.size());
    out.decoded = DecodeOne(out.Source(), pc, timing, services.model);
    if (!out.decoded) {
        if (out.decoded.error == DecodeError::Truncated &&
            fetched.access.status != AccessStatus::Completed) {
            out.failure.kind = fetched.access.status == AccessStatus::Fault ? StepKind::Fault
                                                                            : StepKind::Unsupported;
            out.failure.fault_address = fetched.access.fault_address;
            return out;
        }
        out.failure.kind =
                out.decoded.error == DecodeError::UnsupportedGuestFeature ? StepKind::Unsupported
                : out.decoded.error == DecodeError::Truncated             ? StepKind::Truncated
                                                              : StepKind::InvalidEncoding;
    }
    return out;
}
}  // namespace

StepResult InterpretDecoded(const Instruction& decoded,
                            std::span<const std::uint8_t> source,
                            arch::GuestState& state,
                            const DirectServices& services) {
    StepResult result;
    result.pc = result.next_pc = state.pc;
    const auto& instruction = decoded.View().Record();
    if (instruction.addr != state.pc || !instruction.size || instruction.size > 15 ||
        instruction.flags == InstructionFlag::Invalid) {
        result.kind = StepKind::InvalidEncoding;
        return result;
    }
    if (source.size() < instruction.size) {
        result.kind = StepKind::Truncated;
        return result;
    }
    using Sem = detail::Semantics<DirectSink>;
    DirectSink sink{state, services};
    StateLayout layout;
    layout.gprs_offset = offsetof(arch::GuestState, gprs);
    layout.pc_offset = offsetof(arch::GuestState, pc);
    layout.fprs_offset = offsetof(arch::GuestState, xmm);
    layout.fs_base_offset = offsetof(arch::GuestState, fs_base);
    layout.gs_base_offset = offsetof(arch::GuestState, gs_base);
    layout.mxcsr_offset = offsetof(arch::GuestState, mxcsr);
    layout.x87_offset = offsetof(arch::GuestState, x87);
    layout.cpu_query = services.cpuid ? 1 : 0;
    layout.system_query = reinterpret_cast<std::uintptr_t>(&arch::QueryGuestSystem);
    layout.runtime_state_services = services.state_image != nullptr;
    layout.x87_ip_offset = offsetof(arch::GuestState, x87_ip);
    layout.x87_dp_offset = offsetof(arch::GuestState, x87_dp);
    layout.x87_opcode_offset = offsetof(arch::GuestState, x87_opcode);
    layout.ymm_high_offset = offsetof(arch::GuestState, ymm_high);
    const auto next = state.pc + instruction.size;
    Sem::PendingFlags pending;
    const auto finish = [&](u64 pc) {
        pending.Flush(sink);
        state.pc = pc;
    };
    const auto branch = [&](DirectSink::Value condition, u64 yes, u64 no) {
        finish(condition.number.bits ? yes : no);
    };

    struct Flow {
        decltype(finish)& finish;
        decltype(branch)& branch;
    } flow{finish, branch};

    bool ended{};
    try {
        if (!Sem::AllowedPrefixes(instruction, source))
            throw detail::Rejected{BuildError::Unsupported};
        Sem::Execute(sink,
                     layout,
                     instruction,
                     source.first(instruction.size),
                     pending,
                     result.pc,
                     next,
                     {},
                     flow,
                     ended,
                     InstructionRange{std::span<const Instruction>{&decoded, 1}},
                     source);
        pending.Flush(sink);
        if (!ended) state.pc = next;
        result.kind = sink.exit_service ? StepKind::Service : StepKind::Completed;
        result.service = sink.exit_service;
        result.instructions = 1;
        result.decoded_bytes = instruction.size;
    } catch (const detail::Rejected&) {
        result.kind = sink.observed_effects ? StepKind::Incomplete : StepKind::Unsupported;
    } catch (const direct::Exit& exit) {
        result.kind = exit.kind == direct::ExitKind::Fault     ? StepKind::Fault
                      : exit.kind == direct::ExitKind::Service ? StepKind::Service
                      : sink.observed_effects                  ? StepKind::Incomplete
                                                               : StepKind::Unsupported;
        result.fault_address = exit.address;
        result.service = exit.service;
    }
    result.next_pc = state.pc;
    result.code_changed = sink.code_changed;
    return result;
}

StepResult InterpretOne(arch::GuestState& state,
                        const DirectServices& services,
                        DecodeTiming* timing) {
    const auto fetched = FetchInstruction(state.pc, services, timing);
    if (!fetched.decoded) return fetched.failure;
    return InterpretDecoded(fetched.decoded.instruction, fetched.Source(), state, services);
}

StepResult InterpretBlock(arch::GuestState& state,
                          const DirectServices& services,
                          std::size_t max_instructions,
                          DecodeTiming* timing,
                          ReadyEntryLookup ready) {
    StepResult result;
    result.pc = result.next_pc = state.pc;
    if (!max_instructions) {
        result.kind = StepKind::Unsupported;
        return result;
    }
    for (std::size_t i = 0; i < max_instructions; ++i) {
        if (i && ready.Contains(state.pc)) return result;
        const auto fetched = FetchInstruction(state.pc, services, timing);
        auto step =
                fetched.decoded
                        ? InterpretDecoded(
                                  fetched.decoded.instruction, fetched.Source(), state, services)
                        : fetched.failure;
        step.instructions += result.instructions;
        step.decoded_bytes += result.decoded_bytes;
        result = step;
        if (!fetched.decoded || result.kind != StepKind::Completed || result.code_changed)
            return result;
        const auto& record = fetched.decoded.instruction.View().Record();
        if (record.unit_boundary || record.flow != FlowKind::Linear ||
            result.next_pc != record.addr + record.size)
            return result;
    }
    return result;
}

}  // namespace swift::x86
