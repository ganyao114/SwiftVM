namespace {
u32 UnsignedEstimateReference(u32 value, u32 operation) {
    using Big = boost::multiprecision::cpp_int;
    if (value < (operation == 0 ? 0x80000000u : 0x40000000u)) return ~u32{0};
    const u32 shift = operation == 0 || value < 0x80000000u ? 23 : 24;
    const Big denominator = 2 * (value >> shift) + 1;
    const Big numerator = operation == 0 ? Big{262144}
        : Big{65536} * (Big{1} << (33 - shift));
    Big estimate;
    if (operation == 0) estimate = (2 * numerator + denominator) / (2 * denominator);
    else {
        const Big quotient = numerator / denominator;
        estimate = sqrt(quotient);
        if ((2 * estimate + 1) * (2 * estimate + 1) * denominator < 4 * numerator) ++estimate;
    }
    return estimate.convert_to<u32>() << 23;
}
}

TEST_CASE("ARM64 unsigned estimates match independent fixed-point rational roots and preserve FP state") {
    u64 seed = 0xF9216A083D2C47B0;
    for (u32 operation = 0; operation < 2; ++operation) for (bool q : {false, true}) {
        for (auto regs : Aliases) {
            const u32 instruction = (operation == 0 ? NEON_URECPE : NEON_URSQRTE)
                | (q ? NEON_Q : 0) | (regs[1] << 5) | regs[0];
            Program program(instruction);
            for (u32 trial = 0; trial < 1792; ++trial) {
                Machine machine;
                HighVectorBits source{Random(seed), Random(seed)}, wanted{};
                if (trial < 1536) {
                    const u32 interval = trial / 3;
                    const u32 value = (interval << 23) + (trial % 3) - 1;
                    PutElement(source, 32, 0, value);
                    PutElement(source, 32, 1, value ^ 0x80000000u);
                }
                machine.SetVector(regs[1], source);
                // Integer estimates accept controls unsupported by FP arithmetic.
                machine.Context().fpcr = trial & 1 ? 0x87c09f07u : 0;
                machine.Context().fpsr = trial & 2 ? 0x0800009fu : 0;
                const auto before = machine.Context();
                for (u32 lane = 0; lane < (q ? 4u : 2u); ++lane)
                    PutElement(wanted, 32, lane,
                        UnsignedEstimateReference(u32(Element(source, 32, lane)), operation));
                program.Run(machine);
                REQUIRE(machine.GetVector(regs[0]) == wanted);
                REQUIRE(machine.Context().fpcr == before.fpcr);
                REQUIRE(machine.Context().fpsr == before.fpsr);
                REQUIRE(machine.Context().r == before.r);
                REQUIRE(machine.state->host_cpu_flags == 0xB0000000);
                for (u32 reg = 0; reg < 32; ++reg) if (reg != regs[0])
                    REQUIRE(std::memcmp(&machine.Context().v[reg], &before.v[reg], 16) == 0);
            }
        }
    }
}

TEST_CASE("ARM64 unsigned estimates reject reserved size without touching architectural state") {
    for (u32 operation = 0; operation < 2; ++operation) for (bool q : {false, true}) {
        const u32 instruction = (operation == 0 ? NEON_URECPE : NEON_URSQRTE)
            | (q ? NEON_Q : 0) | (1u << 22) | (31u << 5);
        Program program(instruction);
        Machine machine;
        machine.Context().pc = 0x8877665544332211;
        const auto before = machine.Context();
        program.Run(machine, swift::arm64::InterruptReason::ILL_CODE);
        auto wanted = before;
        wanted.padding = u32(swift::arm64::InterruptReason::ILL_CODE);
        REQUIRE(std::memcmp(&machine.Context(), &wanted, sizeof(wanted)) == 0);
        REQUIRE(machine.state->current_loc.Value() == 0x1000);
    }
}
