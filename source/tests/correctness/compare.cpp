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

bool NondeterministicForm(const Case& c) {
    if (c.isa != ISA::X86_64) return false;
    const auto id = std::string_view(c.form_id);
    static constexpr std::string_view names[] = {
            "RDTSC", "RDTSCP", "RDRAND", "RDSEED", "RDFSBASE", "RDGSBASE",
            "WRFSBASE", "WRGSBASE"};
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
    if (inventory && InventoryX87Form(c))
        return {false, false, "x87_gap", "x87 state and semantics are not oracle-verified"};
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
        if (actual.outcome == Outcome::Rejected)
            return {true, false, "unexpected_rejection", actual.stage + ": " + actual.error};
        return {false,
                false,
                std::string(Name(actual.outcome)),
                actual.stage + ": " + actual.error};
    }
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
    if (c.isa == ISA::X86_64 && !inventory) {
        if (expected.state.x87 != actual.state.x87)
            return {true, false, "semantic_mismatch", "x87 80-bit register payload differs"};
        const auto& e = expected.state;
        const auto& a = actual.state;
        if (e.x87_control != a.x87_control)
            return Different("x87_control", e.x87_control, a.x87_control);
        if (e.x87_status != a.x87_status)
            return Different("x87_status", e.x87_status, a.x87_status);
        if (e.x87_tag != a.x87_tag) return Different("x87_tag", e.x87_tag, a.x87_tag);
        if (e.x87_opcode != a.x87_opcode)
            return Different("x87_opcode", e.x87_opcode, a.x87_opcode);
        if (e.x87_ip != a.x87_ip) return Different("x87_ip", e.x87_ip, a.x87_ip);
        if (e.x87_dp != a.x87_dp) return Different("x87_dp", e.x87_dp, a.x87_dp);
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
        if (expected.fault_kind != actual.fault_kind ||
            expected.fault_access != actual.fault_access ||
            expected.fault_vector != actual.fault_vector)
            return {true, false, "exception_mismatch", "fault kind/access/vector differs"};
        if (expected.fault_pc != actual.fault_pc)
            return Different("fault_pc", expected.fault_pc, actual.fault_pc, "exception_mismatch");
        if (expected.fault_address_valid != actual.fault_address_valid)
            return {true, false, "exception_mismatch", "fault address availability differs"};
        const bool execute_fault =
                expected.fault_access == "execute" && actual.fault_access == "execute";
        const bool normalized_fault =
                crossing_write && expected.fault_address_valid &&
                expected.fault_address >= actual.fault_address &&
                expected.fault_address - actual.fault_address < actual.fault_width;
        if (execute_fault) {
            if (!gaps.empty()) gaps += "; ";
            gaps += "instruction-fetch width is not architectural";
        }
        if (!execute_fault && !normalized_fault && expected.fault_width != actual.fault_width)
            return Different("fault_width",
                             expected.fault_width,
                             actual.fault_width,
                             "exception_mismatch");
        if (!normalized_fault && expected.fault_address != actual.fault_address)
            return Different("fault_address",
                             expected.fault_address,
                             actual.fault_address,
                             "exception_mismatch");
    }
    return {true, true, "agreement", {}, gaps};
}

}  // namespace swift::tests::correctness
