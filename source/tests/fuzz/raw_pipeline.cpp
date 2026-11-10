#include "tests/correctness/execute.h"

#include <cstdlib>

namespace test = swift::tests::correctness;

// This explores the entire decoder/lowering surface, including unreviewed
// forms. Rejection is ordinary; it supplies robustness evidence, not semantic
// coverage. Generated code is never executed for arbitrary input bytes.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2 || size > 257) return 0;
    const auto choice = data[0];
    test::Case c;
    c.isa = static_cast<test::ISA>(1 + choice % 3);
    c.code_address = c.initial.pc = 0x04000000;
    c.initial.flags = c.isa == test::ISA::X86_64 ? 0x202
                      : c.isa == test::ISA::Arm32 ? 0x10 | ((choice & 8) ? 32 : 0)
                                                  : 0;
    c.initial.fp_control = c.isa == test::ISA::X86_64 ? 0x1f80 : 0;
    c.code.assign(data + 1, data + size);
    c.terminal_pc = c.code_address + c.code.size();
    test::RunConfig config;
    config.backend = static_cast<test::Backend>(1 + ((choice >> 4) % 4));
    config.scope = static_cast<test::Scope>((choice >> 6) & 1);
    config.target = static_cast<test::TargetMode>((choice >> 2) % 3);
    config.optimize = choice & 128;
    config.compile_only = true;
    const auto result = test::ExecuteCompile(c, config);
    if (result.outcome == test::Outcome::Rejected &&
        (result.stage == "optimize" ||
         (result.stage == "compile" && result.error.starts_with("backend=1 "))))
        std::abort();
    return 0;
}
