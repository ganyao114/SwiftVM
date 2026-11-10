#include "execute.h"
#include "arch/state.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include "base/format.h"
#include "runtime/arch/arm32/location.h"
#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm32/service.h"
#include "runtime/exit.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "runtime/frontend/arm64/interpreter.h"
#include "runtime/frontend/x86/interpreter.h"
#include "runtime/ir/direct/interpreter.h"

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
    std::uint64_t reservation_address{}, reservation_bytes{};

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

    static bool ByteReadable(const Case& c,
                             const std::vector<MemoryRegion>& memory,
                             std::uint64_t address) {
        for (const auto& region : memory)
            if ((region.protection & 1) && address >= region.address &&
                address - region.address < region.size)
                return true;
        return address >= c.code_address && address - c.code_address < c.code.size();
    }

    static bool ByteWritable(const std::vector<MemoryRegion>& memory, std::uint64_t address) {
        for (const auto& region : memory)
            if ((region.protection & 2) && address >= region.address &&
                address - region.address < region.size)
                return true;
        return false;
    }

    static std::uint64_t FirstUnreadable(const Case& c,
                                         const std::vector<MemoryRegion>& memory,
                                         std::uint64_t address,
                                         std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            if (!ByteReadable(c, memory, address + i)) return address + i;
        return address + size;
    }

    static std::uint64_t FirstUnwritable(const std::vector<MemoryRegion>& memory,
                                         std::uint64_t address,
                                         std::size_t size) {
        for (std::size_t i = 0; i < size; ++i)
            if (!ByteWritable(memory, address + i)) return address + i;
        return address + size;
    }

    static AccessResult Read(void* raw,
                             std::uint64_t address,
                             std::span<std::uint8_t> out,
                             bool) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        arena.access = "read";
        arena.width = out.size();
        const auto& c = arena.input;
        const auto missing = FirstUnreadable(c, arena.memory, address, out.size());
        if (missing != address + out.size()) return {AccessStatus::Fault, missing};
        for (std::size_t i = 0; i < out.size(); ++i) {
            const auto byte_address = address + i;
            bool found{};
            for (const auto& region : arena.memory)
                if ((region.protection & 1) && byte_address >= region.address &&
                    byte_address - region.address < region.size) {
                    out[i] = region.bytes[byte_address - region.address];
                    found = true;
                    break;
                }
            if (!found) out[i] = c.code[byte_address - c.code_address];
        }
        return {};
    }

    static int StateService(const Case& c,
                            runtime::arm32::GuestState& state,
                            Arena& arena,
                            Observation& out) {
        using Kind = runtime::arm32::StateServiceKind;
        const auto request = runtime::arm32::DecodeStateService(state.service_word);
        const auto complete = [&] {
            state.pc = runtime::arm32::Location{state.service_pc}
                               .Advance(request.instruction_bytes)
                               .AdvanceIT()
                               .Key();
            return 0;
        };
        const auto fault = [&](std::uint64_t address, unsigned bytes, bool alignment) {
            out.outcome = Outcome::Fault;
            out.fault_kind = alignment ? "alignment" : "memory";
            out.fault_access = request.kind == Kind::StoreExclusive ? "write" : "read";
            out.fault_pc = static_cast<std::uint32_t>(state.pc);
            out.fault_address = address;
            out.fault_width = bytes;
            out.fault_vector = -1;
            out.fault_address_valid = true;
            return 1;
        };
        if (request.kind == Kind::LoadExclusive || request.kind == Kind::StoreExclusive) {
            const auto bytes = 1u << request.size_log2;
            const auto address = std::uint64_t(std::uint32_t(
                    state.r[request.address_register] + 4u * request.offset_words));
            const bool matching = state.reservation_valid && arena.reservation_address == address &&
                                  arena.reservation_bytes == bytes;
            state.reservation_valid = 0;
            if (request.kind == Kind::StoreExclusive && !matching) {
                state.r[request.status_register] = 1;
                return complete();
            }
            if (address & (bytes - 1)) return fault(address, bytes, true);
            if (request.kind == Kind::LoadExclusive) {
                std::array<std::uint8_t, 8> bytes_buffer{};
                const auto result = Read(&arena, address, {bytes_buffer.data(), bytes}, false);
                if (result.status != AccessStatus::Completed) return fault(address, bytes, false);
                std::uint64_t value = 0;
                for (unsigned i = 0; i < bytes; ++i)
                    value |= std::uint64_t(bytes_buffer[i]) << (8 * i);
                state.r[request.first_register] = std::uint32_t(value);
                if (bytes == 8) state.r[request.second_register] = std::uint32_t(value >> 32);
                arena.reservation_address = address;
                arena.reservation_bytes = bytes;
                state.reservation_valid = 1;
                return complete();
            }
            std::uint64_t desired = std::uint32_t(state.r[request.first_register]);
            if (bytes == 8)
                desired |= std::uint64_t(std::uint32_t(state.r[request.second_register])) << 32;
            if (bytes < 8) desired &= (std::uint64_t{1} << (bytes * 8)) - 1;
            std::array<std::uint8_t, 8> bytes_buffer{};
            for (unsigned i = 0; i < bytes; ++i)
                bytes_buffer[i] = std::uint8_t(desired >> (8 * i));
            const auto result = Write(&arena, address, {bytes_buffer.data(), bytes}, false);
            if (result.status != AccessStatus::Completed) return fault(address, bytes, false);
            state.r[request.status_register] = 0;
            return complete();
        }
        if (request.kind == Kind::ClearExclusive) {
            state.reservation_valid = 0;
            return complete();
        }
        if (request.kind == Kind::ReadTLS) {
            state.r[request.first_register] = request.offset_words ? state.tpidruro : state.tpidrurw;
            return complete();
        }
        if (request.kind == Kind::WriteTLS) {
            state.tpidrurw = std::uint32_t(state.r[request.first_register]);
            return complete();
        }
        if (request.kind == Kind::WriteCPSR) {
            const auto fields = request.offset_words;
            const std::uint32_t mask = ((fields & 8) ? 0xf8000000u : 0u) |
                                       ((fields & 4) ? 0x000f0000u : 0u) |
                                       ((fields & 2) ? 0x00000200u : 0u);
            const auto cpsr = (state.Cpsr() & ~mask) |
                              (std::uint32_t(state.r[request.first_register]) & mask);
            if (cpsr & 512u) {
                out.outcome = Outcome::Rejected;
                out.error = "ARM32 big-endian execution is unsupported";
                return 2;
            }
            state.SetCpsr(cpsr);
            return complete();
        }
        if (request.kind == Kind::WriteFPSCR) {
            const auto value = std::uint32_t(state.r[request.first_register]);
            if (!runtime::arm32::IsSupportedFPSCR(value)) {
                out.outcome = Outcome::Rejected;
                out.error = "ARM32 FPSCR exception enables are unsupported";
                return 2;
            }
            state.fpscr = value;
            state.RekeyFPSCR();
            return complete();
        }
        if (request.kind == Kind::CacheFlush) return complete();
        out.outcome = Outcome::Rejected;
        out.error = "unknown ARM32 state service";
        return 2;
    }

    static AccessResult Atomic(void* raw,
                               const runtime::AtomicRequest& request,
                               runtime::AtomicValue& observed) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        const auto bytes = request.bits / 8u;
        if (!bytes || bytes > 16) return {AccessStatus::UnsupportedBeforeAccess, request.address};
        // A faulting read-modify-write reports the read side; the oracle does
        // not project a distinct atomic access kind.
        arena.access = "read";
        arena.width = static_cast<std::uint32_t>(bytes);
        for (std::size_t i = 0; i < bytes; ++i) {
            const auto address = request.address + i;
            if (!ByteWritable(arena.memory, address) || !ByteReadable(arena.input, arena.memory, address))
                return {AccessStatus::Fault, address};
        }
        std::uint64_t old_low = 0, old_high = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            const auto address = request.address + i;
            std::uint8_t byte = 0;
            for (const auto& region : arena.memory)
                if ((region.protection & 1) && address >= region.address &&
                    address - region.address < region.size) {
                    byte = region.bytes[address - region.address];
                    break;
                }
            if (i < 8) old_low |= std::uint64_t(byte) << (i * 8);
            else old_high |= std::uint64_t(byte) << ((i - 8) * 8);
        }
        observed.low = old_low;
        observed.high = old_high;
        std::uint64_t desired_low = request.desired.low, desired_high = request.desired.high;
        switch (request.operation) {
            case runtime::AtomicOperation::Exchange: break;
            case runtime::AtomicOperation::StoreExclusive:
                if (old_low != request.expected.low) return {};
                break;
            case runtime::AtomicOperation::CompareExchange:
                if (old_low != request.expected.low) return {};
                break;
            case runtime::AtomicOperation::CompareExchange128:
                if (old_low != request.expected.low || old_high != request.expected.high) return {};
                break;
            case runtime::AtomicOperation::FetchAdd:
                desired_low = old_low + request.desired.low;
                break;
            case runtime::AtomicOperation::Add:
                desired_low = old_low + request.desired.low + request.carry;
                break;
            case runtime::AtomicOperation::Sub:
                desired_low = old_low - request.desired.low;
                break;
            case runtime::AtomicOperation::And: desired_low = old_low & request.desired.low; break;
            case runtime::AtomicOperation::Or: desired_low = old_low | request.desired.low; break;
            case runtime::AtomicOperation::Xor: desired_low = old_low ^ request.desired.low; break;
            case runtime::AtomicOperation::Neg: desired_low = 0 - old_low; break;
            case runtime::AtomicOperation::AddCarry:
            case runtime::AtomicOperation::SubBorrow:
                return {AccessStatus::UnsupportedBeforeAccess, request.address};
            case runtime::AtomicOperation::SignedMax: {
                const auto a = static_cast<std::int64_t>(old_low);
                const auto b = static_cast<std::int64_t>(request.desired.low);
                desired_low = static_cast<std::uint64_t>(a > b ? a : b);
                break;
            }
            case runtime::AtomicOperation::SignedMin: {
                const auto a = static_cast<std::int64_t>(old_low);
                const auto b = static_cast<std::int64_t>(request.desired.low);
                desired_low = static_cast<std::uint64_t>(a < b ? a : b);
                break;
            }
            case runtime::AtomicOperation::UnsignedMax:
                desired_low = old_low > request.desired.low ? old_low : request.desired.low;
                break;
            case runtime::AtomicOperation::UnsignedMin:
                desired_low = old_low < request.desired.low ? old_low : request.desired.low;
                break;
        }
        const auto mask = request.bits >= 64 ? UINT64_MAX : (std::uint64_t(1) << request.bits) - 1;
        desired_low &= mask;
        if (bytes > 8) desired_high &= mask;
        for (std::size_t i = 0; i < bytes; ++i) {
            const auto address = request.address + i;
            const auto value = i < 8 ? std::uint8_t(desired_low >> (i * 8))
                                     : std::uint8_t(desired_high >> ((i - 8) * 8));
            for (auto& region : arena.memory)
                if ((region.protection & 2) && address >= region.address &&
                    address - region.address < region.size) {
                    region.bytes[address - region.address] = value;
                    break;
                }
        }
        return {};
    }

    static AccessResult Write(void* raw,
                              std::uint64_t address,
                              std::span<const std::uint8_t> in,
                              bool) noexcept {
        auto& arena = *static_cast<Arena*>(raw);
        arena.access = "write";
        arena.width = in.size();
        const auto missing = FirstUnwritable(arena.memory, address, in.size());
        if (missing != address + in.size()) return {AccessStatus::Fault, missing};
        for (std::size_t i = 0; i < in.size(); ++i) {
            const auto byte_address = address + i;
            for (auto& region : arena.memory)
                if ((region.protection & 2) && byte_address >= region.address &&
                    byte_address - region.address < region.size) {
                    region.bytes[byte_address - region.address] = in[i];
                    break;
                }
        }
        return {};
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
    services.atomic = Arena::Atomic;
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
            if constexpr (std::is_same_v<GuestState, runtime::arm32::GuestState>) {
                if (reason == runtime::NativeExit::StateService) {
                    const auto service = Arena::StateService(c, state, arena, out);
                    if (service != 0) break;
                    continue;
                }
            }
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
        const auto operation = runtime::ir::direct::LastUnsupportedOperation();
        out.error = Format("direct step kind {} at {:#x}{}",
                           unsigned(step.kind),
                           step.pc,
                           operation ? std::string(" opcode=") + operation : std::string{});
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
