#include "execute.h"
#include "arch/state.h"

#include <algorithm>
#include <cstring>
#include "base/format.h"
#include "runtime/exit.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "runtime/frontend/arm64/interpreter.h"
#include "runtime/frontend/x86/interpreter.h"

namespace swift::tests::correctness {
namespace {
namespace front = runtime::frontend;
using runtime::ir::direct::AccessResult;
using runtime::ir::direct::AccessStatus;

struct Arena {
    const Case& input;
    std::vector<MemoryRegion> memory;
    std::string_view access;
    std::uint32_t width{};

    explicit Arena(const Case& input) : input(input), memory(input.memory) {
        for (auto& region : memory) region.bytes.resize(region.size);
    }

    static bool Contains(std::uint64_t base,
                         std::size_t size,
                         std::uint64_t address,
                         std::size_t count) {
        return address >= base && address - base <= size && count <= size - (address - base);
    }

    static front::CodeReadResult Fetch(void* raw,
                                       std::uint64_t address,
                                       std::span<std::uint8_t> out) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        const auto& c = arena.input;
        if (address < c.code_address || address - c.code_address >= c.code.size())
            return {{AccessStatus::Fault, address}, 0};
        const auto offset = static_cast<std::size_t>(address - c.code_address);
        const auto size = std::min(out.size(), c.code.size() - offset);
        std::memcpy(out.data(), c.code.data() + offset, size);
        return {{}, size};
    }

    static AccessResult Read(void* raw,
                             std::uint64_t address,
                             std::span<std::uint8_t> out,
                             bool) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        arena.access = "read";
        arena.width = out.size();
        for (const auto& region : arena.memory)
            if ((region.protection & 1) &&
                Contains(region.address, region.size, address, out.size())) {
                std::memcpy(out.data(), region.bytes.data() + (address - region.address), out.size());
                return {};
            }
        if (Contains(arena.input.code_address, arena.input.code.size(), address, out.size())) {
            std::memcpy(out.data(),
                        arena.input.code.data() + (address - arena.input.code_address),
                        out.size());
            return {};
        }
        return {AccessStatus::Fault, address};
    }

    static AccessResult Write(void* raw,
                              std::uint64_t address,
                              std::span<const std::uint8_t> in,
                              bool) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        arena.access = "write";
        arena.width = in.size();
        for (auto& region : arena.memory)
            if ((region.protection & 2) &&
                Contains(region.address, region.size, address, in.size())) {
                std::memcpy(region.bytes.data() + (address - region.address), in.data(), in.size());
                return {};
            }
        return {AccessStatus::Fault, address};
    }
};

std::uint64_t Physical(ISA isa, std::uint64_t pc) {
    return isa == ISA::Arm32 ? std::uint32_t(pc) : pc;
}

void Fault(Observation& out,
           const Case& c,
           const front::StepResult& step,
           const Arena& arena) {
    out.outcome = Outcome::Fault;
    out.fault_pc = Physical(c.isa, step.pc);
    out.fault_address = step.fault_address;
    out.fault_kind = "memory";
    out.fault_access = arena.access.empty() ? "execute" : arena.access;
    out.fault_width = arena.width;
    out.fault_address_valid = true;
    out.fault_vector = c.isa == ISA::X86_64 ? 14 : -1;
    const auto reason = static_cast<runtime::NativeExit>(step.service & 255);
    if (reason == runtime::NativeExit::DivideFault) {
        out.fault_kind = "divide";
        out.fault_vector = c.isa == ISA::X86_64 ? 0 : -1;
        out.fault_address_valid = false;
        out.fault_access.clear();
    } else if (reason == runtime::NativeExit::AlignmentFault) {
        out.fault_kind = "alignment";
        out.fault_vector = c.isa == ISA::X86_64 ? 13 : -1;
    } else if (reason == runtime::NativeExit::FloatingFault ||
               reason == runtime::NativeExit::X87Fault) {
        out.fault_kind = "floating_point";
        out.fault_vector = c.isa == ISA::X86_64
                                   ? (reason == runtime::NativeExit::X87Fault ? 16 : 19)
                                   : -1;
        out.fault_address_valid = false;
        out.fault_access.clear();
    }
}

template <class GuestState, class Services, class Step>
Observation Run(const Case& c, GuestState& state, Services services, Step step_one) {
    Arena arena{c};
    services.opaque = &arena;
    services.fetch = Arena::Fetch;
    services.read = Arena::Read;
    services.write = Arena::Write;
    Observation out;
    out.backend = "direct";
    out.stage = "execute";
    out.outcome = Outcome::Budget;
    Inject(c.initial, state);
    for (std::uint32_t iteration = 0; iteration < c.max_steps; ++iteration) {
        if (Physical(c.isa, state.pc) == c.terminal_pc) {
            out.outcome = Outcome::Terminal;
            break;
        }
        arena.access = {};
        arena.width = 0;
        const auto step = step_one(state, services);
        out.steps += step.instructions;
        if (step.kind == front::StepKind::Completed) continue;
        if (step.kind == front::StepKind::Fault) {
            Fault(out, c, step, arena);
            break;
        }
        if (step.kind == front::StepKind::Service) {
            const auto reason = static_cast<runtime::NativeExit>(step.service & 255);
            if (reason == runtime::NativeExit::FPEnvironment) continue;
            if (reason == runtime::NativeExit::SemanticException) {
                out.outcome = Outcome::Fault;
                out.fault_kind = ((step.service >> 8) & 255) ==
                                                 unsigned(runtime::GuestExceptionKind::Breakpoint)
                                         ? "breakpoint"
                                         : "illegal_instruction";
                out.fault_pc = Physical(c.isa, step.pc);
                out.fault_vector = c.isa == ISA::X86_64
                                           ? (out.fault_kind == "breakpoint" ? 3 : 6)
                                           : -1;
            } else if (reason == runtime::NativeExit::DivideFault ||
                       reason == runtime::NativeExit::AlignmentFault ||
                       reason == runtime::NativeExit::FloatingFault ||
                       reason == runtime::NativeExit::X87Fault) {
                Fault(out, c, step, arena);
            } else {
                out.outcome = Outcome::Service;
                out.service = step.service;
            }
            break;
        }
        if (step.kind == front::StepKind::InvalidEncoding) {
            out.outcome = Outcome::Fault;
            out.fault_kind = "illegal_instruction";
            out.fault_pc = Physical(c.isa, step.pc);
            out.fault_vector = c.isa == ISA::X86_64 ? 6 : -1;
            break;
        }
        out.outcome = Outcome::Rejected;
        out.error = Format("direct step kind {} at {:#x}", unsigned(step.kind), step.pc);
        break;
    }
    if (out.outcome == Outcome::Budget && Physical(c.isa, state.pc) == c.terminal_pc)
        out.outcome = Outcome::Terminal;
    out.state = Project(state);
    out.memory = std::move(arena.memory);
    out.interpreted_steps = out.steps;
    return out;
}

}  // namespace

Observation ExecuteDirect(const Case& c) {
    switch (c.isa) {
        case ISA::X86_64: {
            runtime::x86::GuestState state;
            return Run(c, state, swift::x86::DirectServices{}, [](auto& s, const auto& services) {
                return swift::x86::InterpretOne(s, services);
            });
        }
        case ISA::Arm64: {
            runtime::arm64::GuestState state;
            return Run(c, state, swift::arm64::DirectServices{}, [](auto& s, const auto& services) {
                return swift::arm64::InterpretOne(s, services);
            });
        }
        case ISA::Arm32: {
            runtime::arm32::GuestState state;
            return Run(c, state, swift::arm32::DirectServices{}, [](auto& s, const auto& services) {
                return swift::arm32::InterpretOne(s, services);
            });
        }
    }
    return {};
}

}  // namespace swift::tests::correctness
