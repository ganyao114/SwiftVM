#include "catalog.h"

#include "runtime/frontend/x86/decode.h"

#include <algorithm>
#include <sstream>

namespace swift::tests::correctness {
namespace {

Comparison Different(std::string field,
                     std::uint64_t expected,
                     std::uint64_t actual,
                     std::string category = "semantic_mismatch") {
    std::ostringstream detail;
    detail << field << ": expected=0x" << std::hex << expected << " actual=0x" << actual;
    return {true, false, std::move(category), detail.str()};
}

bool IsExecution(Outcome outcome, const Case& c) {
    return outcome == Outcome::Terminal || outcome == Outcome::Fault ||
           outcome == Outcome::Service ||
           // A one-step inventory case deliberately stops after its single
           // instruction even when a control transfer leaves the linear window.
           (outcome == Outcome::Budget && c.max_steps == 1);
}

bool ExclusiveForm(const Case& c) {
    const auto id = std::string_view(c.form_id);
    return id.find("LDREX") != std::string_view::npos ||
           id.find("LDAEX") != std::string_view::npos ||
           id.find("STREX") != std::string_view::npos ||
           id.find("STLEX") != std::string_view::npos ||
           id.find("_SWP") != std::string_view::npos ||
           id.find("_LDA/") != std::string_view::npos ||
           id.find("_STL/") != std::string_view::npos;
}

bool InventoryX87Form(const Case& c) {
    if (c.isa != ISA::X86_64) return false;
    const auto id = std::string_view(c.form_id);
    if (!id.starts_with("inventory/")) return false;
    const auto guest = id.find("/x86_64/");
    return guest != std::string_view::npos && guest + 8 < id.size() &&
           id[guest + 8] == 'F';
}

bool InventoryMmxForm(const Case& c) {
    if (c.isa != ISA::X86_64) return false;
    const auto id = std::string_view(c.form_id);
    if (!id.starts_with("inventory/")) return false;
    const auto decoded = swift::x86::DecodeOne(std::span<const std::uint8_t>(c.code),
                                               c.code_address);
    if (!decoded) return false;
    for (const auto& operand : decoded.instruction.View().Record().ops)
        if (operand.type == swift::x86::GuestOperandKind::Register &&
            operand.index >= swift::x86::Register::MM0 &&
            operand.index <= swift::x86::Register::MM7)
            return true;
    return false;
}

std::uint64_t RegisterValue(const Case& c, std::uint8_t index, unsigned bits) {
    using namespace swift::x86;
    const auto slot = [&](Register base) -> std::uint64_t {
        return index >= base && index - base < 16 ? c.initial.gpr[index - base] : 0;
    };
    const auto value = bits == 64   ? slot(Register::RAX)
                       : bits == 32 ? slot(Register::EAX)
                       : bits == 16 ? slot(Register::AX)
                                    : slot(Register::AL);
    return bits >= 64 ? value : value & ((std::uint64_t{1} << bits) - 1);
}

bool GuestByte(const Case& c, std::uint64_t address, std::uint8_t& out) {
    for (const auto& region : c.memory)
        if (address >= region.address && address - region.address < region.size) {
            const auto offset = address - region.address;
            out = offset < region.bytes.size() ? region.bytes[offset] : 0;
            return true;
        }
    return false;
}

// BSF/BSR leave the destination undefined when the source is zero; real CPUs
// keep the old destination while emulators may write any value.
std::string_view FormName(const Case& c) {
    // inventory/<isa>/<form>/<index>
    const auto id = std::string_view(c.form_id);
    if (!id.starts_with("inventory/")) return {};
    const auto first = id.find('/', 10);
    if (first == std::string_view::npos) return {};
    const auto second = id.find('/', first + 1);
    return id.substr(first + 1,
                     second == std::string_view::npos ? second : second - first - 1);
}

bool HasVectorMemoryOperand(const Case& c) {
    const auto decoded = swift::x86::DecodeOne(std::span<const std::uint8_t>(c.code),
                                               c.code_address);
    if (!decoded) return false;
    for (const auto& operand : decoded.instruction.View().Record().ops)
        if ((operand.type == swift::x86::GuestOperandKind::Memory ||
             operand.type == swift::x86::GuestOperandKind::IndexedMemory ||
             operand.type == swift::x86::GuestOperandKind::Displacement) &&
            operand.size == 128)
            return true;
    return false;
}

// Legacy (non-VEX) vector forms with a 128-bit memory operand require
// 16-byte alignment; the oracle does not model that #GP. VEX arithmetic
// accepts unaligned operands, but the aligned moves and non-temporal
// stores keep the requirement.
bool LegacyAlignedVectorForm(const Case& c) {
    const auto decoded = swift::x86::DecodeOne(std::span<const std::uint8_t>(c.code),
                                               c.code_address);
    if (!decoded) return false;
    if (!HasVectorMemoryOperand(c)) return false;
    const auto name = FormName(c);
    static constexpr std::string_view unaligned[] = {
            "MOVUPS", "MOVUPD", "LDDQU", "MOVDQU", "MOVSD", "MOVSS", "MOVLPS",
            "MOVHPS", "MOVLPD", "MOVHPD", "MOVQ",   "MOVD",  "MASKMOVDQU",
            "MASKMOVQ"};
    if (decoded.instruction.View().Record().vector_bytes) {
        static constexpr std::string_view aligned_vex[] = {
                "VMOVAPS", "VMOVAPD", "VMOVDQA", "VMOVNTPS", "VMOVNTPD", "VMOVNTDQ"};
        for (const auto candidate : aligned_vex)
            if (name == candidate) return true;
        return false;
    }
    for (const auto candidate : unaligned)
        if (name == candidate) return false;
    return true;
}

bool ControlFlowName(std::string_view name) {
    return name == "CALL" || name == "RET" || name == "JMP" ||
           (name.size() <= 4 && !name.empty() && name[0] == 'J');
}

bool SystemName(std::string_view name) {
    static constexpr std::string_view names[] = {
            "CPUID",    "XSAVE", "XSAVE64", "XSAVEOPT", "XSAVEOPT64",
            "XRSTOR",   "XRSTOR64", "Unsupported", "POP", "PUSH"};
    for (const auto candidate : names)
        if (name == candidate) return true;
    return false;
}

bool ControlRegisterMove(const Case& c) {
    std::size_t at{};
    while (at < c.code.size()) {
        const auto byte = c.code[at];
        if (byte == 0x66 || byte == 0x67 || byte == 0xf0 || byte == 0xf2 || byte == 0xf3 ||
            byte == 0x26 || byte == 0x2e || byte == 0x36 || byte == 0x3e || byte == 0x64 ||
            byte == 0x65 || (byte >= 0x40 && byte <= 0x4f))
            ++at;
        else
            break;
    }
    return at + 1 < c.code.size() && c.code[at] == 0x0f && c.code[at + 1] >= 0x20 &&
           c.code[at + 1] <= 0x23;
}

bool PrefixPolicyName(std::string_view name) {
    static constexpr std::string_view names[] = {
            "BSWAP", "CWDE", "SHLD", "SHRD", "STD", "CLD", "LZCNT", "CVTSD2SI", "MOVBE"};
    for (const auto candidate : names)
        if (name == candidate) return true;
    return false;
}

bool A64UnpredictableForm(const Case& c) {
    if (c.code.size() < 4) return false;
    const std::uint32_t word = std::uint32_t(c.code[0]) | (std::uint32_t(c.code[1]) << 8) |
                               (std::uint32_t(c.code[2]) << 16) |
                               (std::uint32_t(c.code[3]) << 24);
    const unsigned rn = (word >> 5) & 31, rt = word & 31, rt2 = (word >> 10) & 31;
    const auto name = FormName(c);
    if (name == "LoadStoreExclusive") {
        // Mirrors the frontend reservation-field checks: reserved bits must
        // use their architecturally required values.
        if ((word & 0x3F000000u) != 0x08000000u || (word & (1u << 23))) return false;
        const unsigned rs = (word >> 16) & 31;
        const bool load = (word & (1u << 22)) != 0, pair = (word & (1u << 21)) != 0;
        return (pair && !(word & (1u << 31))) || (!pair && rt2 != 31) || (load && rs != 31) ||
               (load && pair && rt == rt2) ||
               (!load && (rs == rt || (pair && rs == rt2) || (rs == rn && rn != 31)));
    }
    if (name == "LoadStorePairPreIndex" || name == "LoadStorePairPostIndex")
        return !(word & (1u << 26)) && rn != 31 && (rn == rt || rn == rt2);
    if (name == "LoadStorePreIndex" || name == "LoadStorePostIndex")
        return rn != 31 && rn == rt;
    return false;
}

bool UndefinedBitScan(const Case& c) {
    if (c.isa != ISA::X86_64) return false;
    const auto id = std::string_view(c.form_id);
    if (!id.starts_with("inventory/") || (id.find("/BSF/") == std::string_view::npos &&
                                          id.find("/BSR/") == std::string_view::npos))
        return false;
    const auto decoded = swift::x86::DecodeOne(std::span<const std::uint8_t>(c.code),
                                               c.code_address);
    if (!decoded) return false;
    const auto& record = decoded.instruction.View().Record();
    const auto& source = record.ops[1];
    if (source.type == swift::x86::GuestOperandKind::Register)
        return RegisterValue(c, source.index, source.size) == 0;
    if (source.type != swift::x86::GuestOperandKind::Memory &&
        source.type != swift::x86::GuestOperandKind::IndexedMemory &&
        source.type != swift::x86::GuestOperandKind::Displacement)
        return false;
    const unsigned address_bits = record.address_bits == 32 ? 32 : 64;
    std::uint64_t address = record.disp;
    if (source.type == swift::x86::GuestOperandKind::Displacement) {
        // Only the displacement contributes.
    } else if (source.index == swift::x86::Register::RIP) {
        address += c.terminal_pc;
    } else if (source.index != swift::x86::Register::None) {
        address += RegisterValue(c, source.index, address_bits);
    }
    if (record.base != swift::x86::Register::None)
        address += RegisterValue(c, record.base, address_bits) * record.scale;
    if (address_bits == 32) address &= 0xffffffffu;
    if (record.segment == swift::x86::Register::FS || record.segment == swift::x86::Register::GS)
        address += c.initial.tls[record.segment == swift::x86::Register::FS ? 0 : 1];
    std::uint64_t value = 0;
    for (unsigned i = 0; i != source.size / 8; ++i) {
        std::uint8_t byte{};
        // Memory beyond the stored case prefix is zero-filled by the arena.
        (void)GuestByte(c, address + i, byte);
        value |= std::uint64_t(byte) << (8 * i);
    }
    return value == 0;
}

bool NondeterministicForm(const Case& c) {
    if (c.isa != ISA::X86_64) return false;
    const auto id = std::string_view(c.form_id);
    static constexpr std::string_view names[] = {
            "RDTSC", "RDTSCP", "RDRAND", "RDSEED", "RDFSBASE", "RDGSBASE",
            "WRFSBASE", "WRGSBASE", "RSQRTSS", "RSQRTPS", "RCPSS", "RCPPS",
            "VRSQRTSS", "VRSQRTPS", "VRCPSS", "VRCPPS"};
    for (const auto name : names)
        if (id.find("/" + std::string(name) + "/") != std::string_view::npos) return true;
    return false;
}

bool EstimateForm(const Case& c) {
    const auto id = std::string_view(c.form_id);
    return id.find("VRECPS") != std::string_view::npos ||
           id.find("VRSQRTS") != std::string_view::npos ||
           id.find("VRECPE") != std::string_view::npos ||
           id.find("VRSQRTE") != std::string_view::npos;
}

}  // namespace

Comparison Compare(const Case& c, const Observation& expected, const Observation& actual) {
    std::string reason;
    if (!ValidateCase(c, &reason)) return {false, false, "unreviewed_contract", reason};
    const bool inventory = std::string_view(c.form_id).starts_with("inventory/");
    if (inventory &&
        (InventoryMmxForm(c) || (InventoryX87Form(c) && actual.backend != "native-x86_64")))
        return {false, false, "x87_gap", "x87/MMX state and semantics are not oracle-verified"};
    if (inventory && NondeterministicForm(c))
        return {false, false, "nondeterministic_gap",
                "time/random/base instruction has no deterministic oracle"};
    if (!IsExecution(expected.outcome, c))
        return {false, false, "oracle_inconclusive", expected.stage + ": " + expected.error};
    if (!IsExecution(actual.outcome, c)) {
        if (actual.outcome == Outcome::Rejected &&
            (actual.error.find("FPSCR exception enables") != std::string::npos ||
             actual.error.find("big-endian") != std::string::npos))
            return {false, false, "profile_gap", actual.error};
        if (actual.outcome == Outcome::Rejected) {
            if (inventory && c.isa == ISA::X86_64) {
                const auto name = FormName(c);
                if (ControlFlowName(name))
                    return {false, false, "control_one_step_gap",
                            "single-instruction control transfer is outside the Direct window"};
                if (SystemName(name) || ControlRegisterMove(c))
                    return {false, false, "unsupported_guest_feature_gap",
                            "privileged or extended system state is not modeled"};
                if (PrefixPolicyName(name))
                    return {false, false, "prefix_policy_gap",
                            "redundant prefix combination is rejected by policy"};
            }
            if (inventory && c.isa == ISA::Arm64) {
                if (A64UnpredictableForm(c))
                    return {false, false, "unpredictable_encoding_gap",
                            "reserved/unpredictable encoding is rejected by the frontend"};
                if (FormName(c).starts_with("LoadStorePAC"))
                    return {false, false, "unsupported_guest_feature_gap",
                            "PAC keys are not modeled"};
            }
            return {true, false, "unexpected_rejection", actual.stage + ": " + actual.error};
        }
        return {false,
                false,
                std::string(Name(actual.outcome)),
                actual.stage + ": " + actual.error};
    }
    if (inventory && c.isa == ISA::X86_64 && actual.outcome == Outcome::Fault &&
        actual.fault_kind == "alignment" && LegacyAlignedVectorForm(c))
        return {false, false, "oracle_alignment_gap",
                "oracle does not model legacy SSE alignment faults"};
    if (inventory && c.isa == ISA::X86_64 && actual.outcome == Outcome::Service &&
        expected.outcome == Outcome::Terminal)
        return {false, false, "oracle_service_gap",
                "oracle does not model the guest service exit"};
    if (inventory && c.isa == ISA::X86_64 && actual.outcome == Outcome::Fault &&
        FormName(c) == "XGETBV")
        return {false, false, "oracle_gp_gap",
                "invalid XCR index #GP projection is not oracle-verified"};
    if (expected.outcome == Outcome::Budget && actual.outcome != Outcome::Budget)
        return {false, false, "oracle_inconclusive",
                "oracle instruction budget exhausted before the contract boundary"};
    if (expected.outcome != actual.outcome) {
        if (c.max_steps == 1 && actual.outcome == Outcome::Budget)
            return {false, false, "control_one_step_gap",
                    "one-step budget is not comparable to the reference boundary"};
        if (ExclusiveForm(c) && actual.outcome == Outcome::Service)
            return {false, false, "state_service_gap",
                    "exclusive state service has no oracle equivalent"};
        return Different("outcome",
                         unsigned(expected.outcome),
                         unsigned(actual.outcome),
                         "exception_mismatch");
    }
    if (inventory && UndefinedBitScan(c))
        return {false, false, "nondeterministic_gap",
                "BSF/BSR source is zero; destination is architecturally undefined"};
    const auto contract = Contract(c);
    bool crossing_write{};
    std::uint64_t partial_begin{}, partial_end{};
    std::string gaps;
    if (contract.fp_control_mask & ~expected.fp_control_verified_mask)
        gaps = "fp-control bits are not oracle-verified";
    if (EstimateForm(c)) {
        if (!gaps.empty()) gaps += "; ";
        gaps += "estimate FPSCR exception flags are implementation-defined";
    }
    if (c.isa == ISA::Arm32 && std::string_view(c.form_id).find("MRS_reg") != std::string_view::npos)
        return {false, false, "oracle_cpsr_gap",
                "MRS CPSR/T-bit projection is not oracle-verified"};
    if (expected.outcome == Outcome::Fault && actual.outcome == Outcome::Fault &&
        actual.fault_access == "write" && actual.fault_address_valid && actual.fault_width > 1) {
        for (const auto& region : actual.memory) {
            if (region.size && region.address + region.size == actual.fault_address) {
                const auto span =
                        std::min<std::uint64_t>(actual.fault_width - 1, region.size);
                partial_begin = actual.fault_address - span;
                partial_end = actual.fault_address;
                crossing_write = true;
                if (!gaps.empty()) gaps += "; ";
                gaps += "cross-boundary write: partial commit and engine fault-address "
                        "granularity are not oracle-verified";
                break;
            }
        }
    }
    if (expected.state.pc != actual.state.pc)
        return Different("pc", expected.state.pc, actual.state.pc);
    const auto flags_expected = expected.state.flags & contract.flags_mask;
    const auto flags_actual = actual.state.flags & contract.flags_mask;
    if (flags_expected != flags_actual) return Different("flags", flags_expected, flags_actual);
    const unsigned gprs = c.isa == ISA::X86_64 ? 16 : c.isa == ISA::Arm64 ? 32 : 15;
    for (unsigned i = 0; i < gprs; ++i) {
        const auto mask = c.isa == ISA::Arm32 ? UINT64_C(0xffffffff) : UINT64_MAX;
        if ((expected.state.gpr[i] & mask) != (actual.state.gpr[i] & mask))
            return Different("gpr[" + std::to_string(i) + "]",
                             expected.state.gpr[i] & mask,
                             actual.state.gpr[i] & mask);
    }
    const unsigned vectors = c.isa == ISA::Arm64 ? 32 : 16;
    unsigned vector_bytes = c.isa == ISA::X86_64 ? 32 : 16;
    if (inventory && c.isa == ISA::X86_64) {
        const auto decoded = swift::x86::DecodeOne(
                std::span<const std::uint8_t>(c.code.data(), c.code.size()), c.code_address);
        if (decoded) {
            const auto effective = decoded.instruction.View().Record().vector_bytes;
            vector_bytes = effective ? effective : 16;
        }
    }
    for (unsigned i = 0; i < vectors; ++i)
        for (unsigned j = 0; j < vector_bytes; ++j)
            if (expected.state.vectors[i][j] != actual.state.vectors[i][j])
                return Different("vector[" + std::to_string(i) + "][" + std::to_string(j) + "]",
                                 expected.state.vectors[i][j],
                                 actual.state.vectors[i][j]);
    const auto fp_control_mask =
            contract.fp_control_mask & expected.fp_control_verified_mask &
            actual.fp_control_verified_mask;
    if ((expected.state.fp_control & fp_control_mask) !=
        (actual.state.fp_control & fp_control_mask))
        return Different("fp_control",
                         expected.state.fp_control & fp_control_mask,
                         actual.state.fp_control & fp_control_mask);
    if ((expected.state.fp_status & contract.fp_status_mask) !=
        (actual.state.fp_status & contract.fp_status_mask))
        return Different("fp_status",
                         expected.state.fp_status & contract.fp_status_mask,
                         actual.state.fp_status & contract.fp_status_mask);
    const unsigned tls = c.isa == ISA::Arm64 ? 1 : 2;
    for (unsigned i = 0; i < tls; ++i)
        if (expected.state.tls[i] != actual.state.tls[i])
            return Different(
                    "tls[" + std::to_string(i) + "]", expected.state.tls[i], actual.state.tls[i]);
    if (c.isa == ISA::X86_64 && (!inventory || actual.backend == "native-x86_64")) {
        if (expected.state.x87 != actual.state.x87)
            return {true, false, "semantic_mismatch", "x87 80-bit register payload differs"};
        const auto& e = expected.state;
        const auto& a = actual.state;
        if (e.x87_control != a.x87_control)
            return Different("x87_control", e.x87_control, a.x87_control);
        if (e.x87_status != a.x87_status)
            return Different("x87_status", e.x87_status, a.x87_status);
        if (e.x87_tag != a.x87_tag) return Different("x87_tag", e.x87_tag, a.x87_tag);
        if (e.x87_opcode != a.x87_opcode) {
            if (actual.backend == "native-x86_64") {
                if (!gaps.empty()) gaps += "; ";
                gaps += "x87 FOP is not projected by the native reference";
            } else
                return Different("x87_opcode", e.x87_opcode, a.x87_opcode);
        }
        if (e.x87_ip != a.x87_ip) {
            if (actual.backend == "native-x86_64") {
                if (!gaps.empty()) gaps += "; ";
                gaps += "x87 FIP is not projected by the native reference";
            } else
                return Different("x87_ip", e.x87_ip, a.x87_ip);
        }
        if (e.x87_dp != a.x87_dp) {
            if (actual.backend == "native-x86_64") {
                if (!gaps.empty()) gaps += "; ";
                gaps += "x87 FDP is not projected by the native reference";
            } else
                return Different("x87_dp", e.x87_dp, a.x87_dp);
        }
    } else if (inventory && InventoryX87Form(c)) {
        if (!gaps.empty()) gaps += "; ";
        gaps += "x87 state projection is not oracle-verified";
    }
    if (expected.memory.size() != actual.memory.size())
        return Different("memory region count", expected.memory.size(), actual.memory.size());
    for (const auto& e : expected.memory) {
        const auto a =
                std::find_if(actual.memory.begin(), actual.memory.end(), [&](const auto& region) {
                    return region.address == e.address;
                });
        if (a == actual.memory.end())
            return {true, false, "semantic_mismatch", "memory region missing"};
        if (e.size != a->size || e.protection != a->protection)
            return {true, false, "semantic_mismatch", "memory region metadata differs"};
        for (std::size_t i = 0; i < e.size; ++i) {
            const auto address = e.address + i;
            if (crossing_write && address >= partial_begin && address < partial_end) continue;
            const auto eb = i < e.bytes.size() ? e.bytes[i] : 0;
            const auto ab = i < a->bytes.size() ? a->bytes[i] : 0;
            if (eb != ab) return Different("memory@" + std::to_string(address), eb, ab);
        }
    }
    if (expected.outcome == Outcome::Service && expected.service != actual.service)
        return Different("service", expected.service, actual.service);
    if (expected.outcome == Outcome::Fault) {
        const auto normalized_access = [](const std::string& access) -> std::string {
            return access.empty() ? "unknown" : access;
        };
        const bool execute_fault =
                expected.fault_access == "execute" && actual.fault_access == "execute";
        if (expected.fault_kind != actual.fault_kind ||
            normalized_access(expected.fault_access) != normalized_access(actual.fault_access) ||
            (!execute_fault && expected.fault_vector != actual.fault_vector))
            return {true, false, "exception_mismatch", "fault kind/access/vector differs"};
        if (expected.fault_pc != actual.fault_pc)
            return Different("fault_pc", expected.fault_pc, actual.fault_pc, "exception_mismatch");
        if (expected.fault_address_valid != actual.fault_address_valid)
            return {true, false, "exception_mismatch", "fault address availability differs"};
        const bool divide_fault =
                expected.fault_kind == "divide" && actual.fault_kind == "divide";
        const bool normalized_fault =
                crossing_write && expected.fault_address_valid &&
                expected.fault_address >= actual.fault_address &&
                expected.fault_address - actual.fault_address < actual.fault_width;
        if (execute_fault) {
            if (!gaps.empty()) gaps += "; ";
            gaps += "instruction-fetch width is not architectural";
        }
        if (!execute_fault && !divide_fault && !normalized_fault &&
            expected.fault_width != actual.fault_width) {
            const auto widest = std::max(expected.fault_width, actual.fault_width);
            const auto name = FormName(c);
            if (c.isa == ISA::Arm64 && widest == 16 &&
                name.find("LoadStorePair") != std::string_view::npos) {
                if (!gaps.empty()) gaps += "; ";
                gaps += "pair-store access width is not architectural";
            } else if (inventory && c.isa == ISA::X86_64 && widest == 16 &&
                       HasVectorMemoryOperand(c)) {
                if (!gaps.empty()) gaps += "; ";
                gaps += "oracle reports the scalar half of a 128-bit access";
            } else
                return Different("fault_width",
                                 expected.fault_width,
                                 actual.fault_width,
                                 "exception_mismatch");
        }
        if (!divide_fault && !normalized_fault && expected.fault_address != actual.fault_address) {
            if (inventory && c.isa == ISA::X86_64 && expected.fault_address_valid &&
                actual.fault_address_valid &&
                (expected.fault_address & 0xffffffffu) ==
                        (actual.fault_address & 0xffffffffu)) {
                if (!gaps.empty()) gaps += "; ";
                gaps += "oracle truncates fault addresses above 32 bits";
            } else
                return Different("fault_address",
                                 expected.fault_address,
                                 actual.fault_address,
                                 "exception_mismatch");
        }
    }
    return {true, true, "agreement", {}, gaps};
}

}  // namespace swift::tests::correctness
