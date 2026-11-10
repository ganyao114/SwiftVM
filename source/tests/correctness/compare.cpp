#include "catalog.h"

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

bool IsExecution(Outcome outcome) {
    return outcome == Outcome::Terminal || outcome == Outcome::Fault || outcome == Outcome::Service;
}

}  // namespace

Comparison Compare(const Case& c, const Observation& expected, const Observation& actual) {
    std::string reason;
    if (!ValidateCase(c, &reason)) return {false, false, "unreviewed_contract", reason};
    if (!IsExecution(expected.outcome))
        return {false, false, "oracle_inconclusive", expected.stage + ": " + expected.error};
    if (!IsExecution(actual.outcome)) {
        if (actual.outcome == Outcome::Rejected)
            return {true, false, "unexpected_rejection", actual.stage + ": " + actual.error};
        return {false,
                false,
                std::string(Name(actual.outcome)),
                actual.stage + ": " + actual.error};
    }
    if (expected.outcome != actual.outcome)
        return Different("outcome",
                         unsigned(expected.outcome),
                         unsigned(actual.outcome),
                         "exception_mismatch");
    const auto contract = Contract(c);
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
    const unsigned vector_bytes = c.isa == ISA::X86_64 ? 32 : 16;
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
    if (c.isa == ISA::X86_64) {
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
            const auto eb = i < e.bytes.size() ? e.bytes[i] : 0;
            const auto ab = i < a->bytes.size() ? a->bytes[i] : 0;
            if (eb != ab) return Different("memory@" + std::to_string(e.address + i), eb, ab);
        }
    }
    if (expected.outcome == Outcome::Service && expected.service != actual.service)
        return Different("service", expected.service, actual.service);
    if (expected.outcome == Outcome::Fault) {
        if (expected.fault_kind != actual.fault_kind ||
            expected.fault_access != actual.fault_access ||
            expected.fault_width != actual.fault_width ||
            expected.fault_vector != actual.fault_vector)
            return {true, false, "exception_mismatch", "fault kind/access/width/vector differs"};
        if (expected.fault_pc != actual.fault_pc)
            return Different("fault_pc", expected.fault_pc, actual.fault_pc, "exception_mismatch");
        if (expected.fault_address_valid != actual.fault_address_valid)
            return {true, false, "exception_mismatch", "fault address availability differs"};
        if (expected.fault_address_valid && expected.fault_address != actual.fault_address)
            return Different("fault_address",
                             expected.fault_address,
                             actual.fault_address,
                             "exception_mismatch");
    }
    return {true, true, "agreement", {}};
}

}  // namespace swift::tests::correctness
