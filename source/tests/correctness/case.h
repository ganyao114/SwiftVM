#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace swift::tests::correctness {

enum class ISA : std::uint8_t { X86_64 = 1, Arm64 = 2, Arm32 = 3 };
enum class Backend : std::uint8_t { Direct = 0, Arm64 = 1, X64 = 2, Riscv64 = 3, LoongArch64 = 4 };
enum class Scope : std::uint8_t { Block = 0, Function = 1 };
enum class TargetMode : std::uint8_t { Known = 0, EarlyOff = 1, Unknown = 2 };
enum class Outcome : std::uint8_t {
    Terminal,
    Fault,
    Service,
    Rejected,
    Budget,
    Unavailable,
    HostFailure,
    Compiled,
};

// Canonical architectural fields, never the in-memory layout of a GuestState.
// x86: gpr order RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15; vectors are YMM.
// A64: gpr[31] is SP; vectors use 16 bytes. A32: r0..r14, PC in pc.
// flags are RFLAGS / NZCV / CPSR. A32 fp_control is the complete FPSCR;
// A64 uses FPCR/FPSR and x86 uses MXCSR in fp_control. Unused fields are zero.
struct State {
    std::array<std::uint64_t, 32> gpr{};
    std::uint64_t pc{}, flags{};
    std::array<std::array<std::uint8_t, 32>, 32> vectors{};
    std::uint32_t fp_control{}, fp_status{};
    std::array<std::uint64_t, 2> tls{};
    std::array<std::array<std::uint8_t, 10>, 8> x87{};
    std::uint16_t x87_control{0x37f}, x87_status{}, x87_tag{0xffff}, x87_opcode{};
    std::uint64_t x87_ip{}, x87_dp{};
};

struct MemoryRegion {
    std::uint64_t address{};
    std::uint32_t size{};
    std::uint8_t protection{3};  // read=1, write=2, execute=4
    std::vector<std::uint8_t> bytes;  // remaining bytes are initially zero
};

struct Case {
    static constexpr std::uint32_t SchemaVersion = 1;
    ISA isa{ISA::X86_64};
    std::string form_id;
    std::string profile;  // x86_64-user / a64-user / armv7-user
    std::uint64_t seed{}, id{};
    std::uint64_t code_address{}, terminal_pc{};
    std::vector<std::uint8_t> code;
    State initial;
    std::vector<MemoryRegion> memory;
    std::uint32_t max_steps{4096};
};

struct RunConfig {
    Backend backend{Backend::Direct};
    Scope scope{Scope::Block};
    TargetMode target{TargetMode::Known};
    bool optimize{true}, pin{true}, compile_only{};
};

struct Observation {
    Outcome outcome{Outcome::HostFailure};
    State state;
    std::vector<MemoryRegion> memory;
    std::string stage, error, backend;
    std::string fault_kind, fault_access;
    std::uint64_t fault_pc{}, fault_address{}, service{}, steps{};
    std::uint32_t fault_width{};
    std::int32_t fault_vector{-1};
    bool fault_address_valid{}, native_executed{};
    std::uint64_t compile_attempts{}, compiled_units{}, interpreted_steps{}, fallbacks{};
    std::uint32_t actual_ir_blocks{}, actual_source_blocks{}, spill_slots{}, frame_bytes{};
    Scope actual_scope{Scope::Block};
};

struct Comparison {
    bool comparable{}, equal{};
    std::string category, detail;
};

std::string_view Name(ISA);
std::string_view Name(Backend);
std::string_view Name(Outcome);
ISA ParseISA(std::string_view);
Backend ParseBackend(std::string_view);

std::vector<std::uint8_t> Encode(const Case&);
Case Decode(std::span<const std::uint8_t>);
Case ReadCase(const std::string&);
void WriteCase(const std::string&, const Case&);
std::string CaseHash(const Case&);
std::string ToJSON(const Case&);
std::string ToJSON(const Observation&);
std::string ToJSON(const RunConfig&);
std::string ToJSON(const Comparison&);
Observation ReadObservation(const std::string&);
RunConfig ReadConfig(const std::string&);

Comparison Compare(const Case&, const Observation& expected, const Observation& actual);
// Predicate must preserve the original configuration and failure category.
Case Reduce(const Case&, const std::function<bool(const Case&)>&, std::uint32_t budget = 256);

}  // namespace swift::tests::correctness
