using X87Reg = swift::x86::X87Reg;
using X87Context = swift::x86::ThreadContext64;
using namespace swift::x86;

struct X87HardwareResult { X87Reg value{}; u16 status{}; u64 compare{}; };

// The reference executes the host's x87 instructions directly, with no
// shared SoftFloat/helper arithmetic. Save even the caller's live x87 stack.
X87HardwareResult X87Hardware(u64 command, X87Reg left, X87Reg right, u16 control, unsigned depth = 2) {
    alignas(16) unsigned char saved[512];
    X87HardwareResult output;
    asm volatile("fxsave64 %0\n\tfninit\n\tfldcw %1" : "=m"(saved) : "m"(control) : "memory",
                 "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    if (depth == 2) asm volatile("fldt %0\n\tfldt %1" : : "m"(right), "m"(left) : "st", "st(1)");
    const auto action = X87Action(command & 0xff);
    const auto operation = (command >> 24) & 0xff;
    if (depth == 9) {
        asm volatile("fld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1\n\tfld1" : : :
                     "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)");
    } else if (action == X87Action::Binary) {
        if ((command >> 32) & X87Pop) {
            switch (X87Binary(operation)) {
                case X87Binary::Add: asm volatile(".byte 0xde, 0xc1" : : : "st", "st(1)"); break;
                case X87Binary::Mul: asm volatile(".byte 0xde, 0xc9" : : : "st", "st(1)"); break;
                case X87Binary::Sub: asm volatile(".byte 0xde, 0xe9" : : : "st", "st(1)"); break;
                case X87Binary::Div: asm volatile(".byte 0xde, 0xf9" : : : "st", "st(1)"); break;
            }
        } else {
            switch (X87Binary(operation)) {
                case X87Binary::Add: asm volatile("fadd %%st(1), %%st" : : : "st"); break;
                case X87Binary::Mul: asm volatile("fmul %%st(1), %%st" : : : "st"); break;
                case X87Binary::Sub: asm volatile("fsub %%st(1), %%st" : : : "st"); break;
                case X87Binary::Div: asm volatile("fdiv %%st(1), %%st" : : : "st"); break;
            }
        }
    } else if (action == X87Action::Unary) {
        switch (X87Unary(operation)) {
            case X87Unary::ChangeSign: asm volatile("fchs" : : : "st"); break;
            case X87Unary::Abs: asm volatile("fabs" : : : "st"); break;
            case X87Unary::Test: asm volatile("ftst" : : : "st"); break;
            case X87Unary::Examine: asm volatile("fxam" : : : "st"); break;
            case X87Unary::Sqrt: asm volatile("fsqrt" : : : "st"); break;
            case X87Unary::Round: asm volatile("frndint" : : : "st"); break;
        }
    } else if (action == X87Action::Compare) {
        u8 carry, parity, zero;
        if ((command >> 32) & X87Unordered)
            asm volatile("fucomi %%st(1), %%st\n\tsetc %0\n\tsetp %1\n\tsetz %2" : "=qm"(carry), "=qm"(parity), "=qm"(zero) : : "cc", "st");
        else
            asm volatile("fcomi %%st(1), %%st\n\tsetc %0\n\tsetp %1\n\tsetz %2" : "=qm"(carry), "=qm"(parity), "=qm"(zero) : : "cc", "st");
        output.compare = carry | (u64(parity) << 1) | (u64(zero) << 2);
    }
    asm volatile("fnstsw %0\n\tfstpt %1\n\tfxrstor64 %2" : "=m"(output.status), "=m"(output.value) : "m"(saved) : "memory",
                 "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)",
                 "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                 "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    return output;
}

IntrusivePtr<ir::Block> X87Program(u64 command, ValueType result_type = ValueType::U64) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd500})}; ir::Assembler as{block.get()};
    auto context = as.GetUniformAddress(ir::Imm{u64{0}}).SetType(ValueType::U64);
    auto address = as.LoadImm(ir::Imm{UINT64_MAX}).SetType(ValueType::U64);
    auto result = as.X87Op(context, ir::Imm{command}, address).SetType(result_type);
    as.StoreUniform(ir::Uniform{2048, result_type}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void X87Seed(X87Context& context, X87Reg left, X87Reg right, u16 control) {
    context = {};
    context.x87_fcw = control; context.x87_fsw = 6 << 11; context.x87_ftw = 0x0fff;
    context.x87_regs[6] = left; context.x87_regs[7] = right;
}

void X87RegisterHardware() {
    const std::array<X87Reg, 10> values{{
        {0, 0}, {0, 0x8000}, {0x8000000000000000ULL, 0x3fff},
        {0xc000000000000000ULL, 0x3fff}, {0xc000000000000000ULL, 0xbfff},
        {0xc000000000000000ULL, 0x4000}, {0x8000000000000000ULL, 0x3ffe},
        {0x8000000000000000ULL, 0x7fff}, {0xc000000000000123ULL, 0x7fff},
        {0x8000000000000123ULL, 0x7fff}}};
    for (u32 precision : {0u, 2u, 3u}) for (u32 rounding = 0; rounding < 4; ++rounding) {
        const u16 control = u16(0x007f | (precision << 8) | (rounding << 10));
        for (u32 operation = 0; operation < 4; ++operation) {
            const auto command = MakeX87Command(X87Action::Binary, X87Format::Register, 1, u8(operation));
            auto block = X87Program(command); Compiled compiled{block.get()};
            for (auto left : values) for (auto right : values) {
                Storage actual; auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
                X87Seed(context, left, right, control);
                const auto expected = X87Hardware(command, left, right, control);
                Check(compiled.fn(actual.state) == HaltReason::CallHost, "register x87 arithmetic halt");
                Check(context.x87_regs[6].significand == expected.value.significand &&
                      context.x87_regs[6].sign_exp == expected.value.sign_exp, "register x87 arithmetic hardware payload");
                Check((context.x87_fsw & 0x7fff) == (expected.status & 0x7fff), "register x87 arithmetic hardware status including C1");
                Check(actual.Get(2048) == 0, "register x87 full U64 zero result");
            }
        }
        for (u32 operation = 0; operation < 6; ++operation) {
            const auto command = MakeX87Command(X87Action::Unary, X87Format::None, 0, u8(operation));
            auto block = X87Program(command); Compiled compiled{block.get()};
            for (auto left : values) {
                Storage actual; auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
                X87Seed(context, left, values[2], control);
                const auto expected = X87Hardware(command, left, values[2], control);
                Check(compiled.fn(actual.state) == HaltReason::CallHost, "register x87 unary halt");
                Check(context.x87_regs[6].significand == expected.value.significand &&
                      context.x87_regs[6].sign_exp == expected.value.sign_exp, "register x87 unary hardware payload");
                Check((context.x87_fsw & 0x7fff) == (expected.status & 0x7fff), "register x87 unary hardware status");
            }
        }
    }
    for (u32 unordered : {0u, u32(X87Unordered)}) {
        const auto command = MakeX87Command(X87Action::Compare, X87Format::Register, 1, 0, X87ToEFlags | unordered);
        auto block = X87Program(command); Compiled compiled{block.get()};
        for (auto left : values) for (auto right : values) {
            Storage actual; auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
            X87Seed(context, left, right, 0x037f);
            const auto expected = X87Hardware(command, left, right, 0x037f);
            Check(compiled.fn(actual.state) == HaltReason::CallHost, "register x87 compare halt");
            Check(actual.Get(2048) == expected.compare, "register x87 Compare preserves compact U64 result");
            Check((context.x87_fsw & 0x7fff) == (expected.status & 0x7fff), "register x87 comparison hardware status");
        }
    }
    std::cout << "PASS register x87 hardware arithmetic / precision / four rounding modes / unary / NaNs\n";
}

void X87HostEnvironmentAndStack() {
    auto block = X87Program(MakeX87Command(X87Action::LoadConstant, X87Format::None, 0, u8(X87Constant::One)));
    Compiled compiled{block.get()}; Storage actual;
    auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
    context.x87_fcw = 0x037f; context.x87_ftw = 0xffff;
    for (u32 variant = 0; variant < 3; ++variant) {
        actual.state->halt_reason = HaltReason::None;
        alignas(16) unsigned char original[512]{}, before[512]{}, after[512]{};
        const u16 control = 0x0b7f; const u32 mxcsr = 0xdfc0;
        asm volatile("fxsave64 %0\n\tfninit\n\tfldz\n\tfldz\n\tfdivp\n\tfldpi\n\tfldcw %2\n\tldmxcsr %3\n\tfxsave64 %1"
                     : "=m"(original), "=m"(before) : "m"(control), "m"(mxcsr) : "memory", "st", "st(1)");
        HaltReason halt;
        if (variant == 0) halt = compiled.fn(actual.state);
        else {
            (void)x64::EvaluateX87Register(actual.state, variant == 1 ? reinterpret_cast<u64>(&context) : u64{1},
                                          variant == 1 ? u64{0xff} : MakeX87Command(X87Action::Init));
            halt = actual.state->halt_reason;
        }
        asm volatile("fxsave64 %0\n\tfxrstor64 %1" : "=m"(after) : "m"(original) : "memory",
                     "st", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)",
                     "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
                     "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
        Check(halt == (variant ? HaltReason::IllegalCode : HaltReason::CallHost), "x87 host environment normal/IllegalCode halt");
        Check((before[2] & 0x3f) != 0 && before[4] != 0, "x87 environment fixture has masked pending exception and live stack");
        Check(std::memcmp(before, after, 28) == 0, "x87 host FCW/FSW/FTW/FIP/FDP/MXCSR preserved");
        for (u32 slot = 0; slot < 8; ++slot)
            Check(std::memcmp(before + 32 + slot * 16, after + 32 + slot * 16, 10) == 0, "x87 host stack payload preserved");
    }
    for (u64 pointer : {u64{0}, u64{1}, UINT64_MAX, reinterpret_cast<u64>(&context) + 16}) {
        IntrusivePtr<ir::Block> invalid{new ir::Block(ir::Location{0xd580})}; ir::Assembler as{invalid.get()};
        auto value = as.LoadImm(ir::Imm{pointer}).SetType(ValueType::U64);
        auto address = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
        auto result = as.X87Op(value, ir::Imm{MakeX87Command(X87Action::Init)}, address).SetType(ValueType::U64);
        as.StoreUniform(ir::Uniform{2048, ValueType::U64}, result); invalid->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled invalid_compiled{invalid.get()}; actual.state->halt_reason = HaltReason::None; actual.Put(2048, UINT64_MAX);
        const auto old_context = context;
        Check(invalid_compiled.fn(actual.state) == HaltReason::IllegalCode, "x87 dynamic context must be current canonical uniform base");
        Check(actual.Get(2048) == UINT64_MAX && std::memcmp(&context, &old_context, sizeof(context)) == 0,
              "x87 invalid context does not dereference or commit guest state/result");
        Check(actual.state->current_loc == ir::Location{0xd580} && actual.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
              "x87 invalid context keeps precise PC and no reader lease");
    }
    for (unsigned depth : {0u, 9u}) {
        IntrusivePtr<ir::Block> stack_block{new ir::Block(ir::Location{0xd600})}; ir::Assembler as{stack_block.get()};
        auto pointer = as.GetUniformAddress(ir::Imm{u64{0}}).SetType(ValueType::U64);
        auto zero = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
        const auto command = depth ? MakeX87Command(X87Action::LoadConstant, X87Format::None, 0, u8(X87Constant::One)) :
                                     MakeX87Command(X87Action::Binary, X87Format::Register, 1, u8(X87Binary::Add));
        for (unsigned i = 0; i < (depth ? depth : 1); ++i) (void)as.X87Op(pointer, ir::Imm{command}, zero).SetType(ValueType::U64);
        auto status = as.X87Op(pointer, ir::Imm{MakeX87Command(X87Action::StoreStatus, X87Format::Register)}, zero).SetType(ValueType::U16);
        as.StoreUniform(ir::Uniform{2048, ValueType::U16}, status); stack_block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled stack_compiled{stack_block.get()}; Storage storage;
        auto& guest = *reinterpret_cast<X87Context*>(storage.state->uniform_buffer_begin);
        guest.x87_fcw = 0x037f; guest.x87_ftw = 0xffff;
        const auto expected = X87Hardware(command, {}, {}, 0x037f, depth);
        Check(stack_compiled.fn(storage.state) == HaltReason::CallHost, "x87 masked stack exception halt");
        Check((storage.Get(2048) & 0x7fff) == (expected.status & 0x7fff), "x87 FNSTSW U16 hardware stack status");
        Check(guest.x87_regs[(guest.x87_fsw >> 11) & 7].significand == expected.value.significand &&
              guest.x87_regs[(guest.x87_fsw >> 11) & 7].sign_exp == expected.value.sign_exp, "x87 stack fault indefinite payload");
    }
    std::cout << "PASS complete host x87 environment and masked stack faults / narrow FNSTSW\n";
}

void X87BinaryRoundingBoundariesAndNaNs() {
    struct Pair { X87Binary operation; X87Reg left, right; };
    const std::array<Pair, 5> pairs{{
        {X87Binary::Div, {0x8000000000000000ULL, 0x3fff}, {0xc000000000000000ULL, 0x4000}},
        {X87Binary::Mul, {0xffffffffffffffffULL, 0x7ffe}, {0x8000000000000000ULL, 0x4000}},
        {X87Binary::Mul, {0x8000000000000001ULL, 0x0001}, {0x8000000000000000ULL, 0x3ffe}},
        {X87Binary::Add, {0x8000000000000000ULL, 0x3fff}, {0x8000000000000000ULL, 0x3fe7}},
        {X87Binary::Add, {0x8000010000000000ULL, 0x3fff}, {0x8000000000000000ULL, 0x3fe7}}
    }};
    for (u32 precision : {0u, 2u, 3u}) for (u32 rounding = 0; rounding < 4; ++rounding)
    for (const auto& pair : pairs) for (bool negative : {false, true}) for (bool pop : {false, true})
    for (bool unmasked : {false, true}) {
        const auto flags = pop ? u32(X87DestIndex | X87Pop) : 0u;
        const auto command = MakeX87Command(X87Action::Binary, X87Format::Register, 1, u8(pair.operation), flags);
        auto left = pair.left, right = pair.right;
        if (negative) { left.sign_exp ^= 0x8000; if (pair.operation == X87Binary::Add) right.sign_exp ^= 0x8000; }
        // ST(1) is the arithmetic destination of popping encodings.
        if (pop) std::swap(left, right);
        const u16 control = u16(0x007f | (precision << 8) | (rounding << 10));
        auto block = X87Program(command); Compiled compiled{block.get()};
        Storage actual; auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
        X87Seed(context, left, right, unmasked ? u16(control & ~0x3f) : control);
        const auto expected = X87Hardware(command, left, right, control);
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "x87 normal/overflow/underflow rounding boundary halt");
        const auto& result = context.x87_regs[pop ? 7 : 6];
        Check(result.significand == expected.value.significand && result.sign_exp == expected.value.sign_exp,
              "x87 rounded boundary and pop hardware payload");
        // Existing guest helper contract publishes ES/B for unmasked guest
        // exceptions while retaining the computed result. Execute the native
        // oracle masked; physical unmasked x87 would suppress that commit.
        const u16 summary = unmasked && (expected.status & 0x3f) ? 0x8080 : 0;
        Check(context.x87_fsw == u16(expected.status | summary),
              "x87 original exception flags/C1/pop and guest unmasked summary contract");
    }
    // The hardware independently arbitrates two NaNs: Q beats S; within
    // either class the larger payload wins, with positive sign breaking ties.
    // Exercise every operand order and every arithmetic operation directly.
    std::array<X87Reg, 8> nans{};
    for (u32 index = 0; index < nans.size(); ++index)
        nans[index] = {0x8000000000000000ULL | ((index & 1) ? 0x4000000000000000ULL : 0) |
                       ((index & 2) ? 0xffff : 1), u16((index & 4) ? 0xffff : 0x7fff)};
    for (u32 operation = 0; operation < 4; ++operation) for (bool pop : {false, true}) {
        const auto command = MakeX87Command(X87Action::Binary, X87Format::Register, 1, u8(operation), pop ? u32(X87DestIndex | X87Pop) : 0u);
        auto block = X87Program(command); Compiled compiled{block.get()};
        for (const auto& left : nans) for (const auto& right : nans) {
            Storage actual; auto& context = *reinterpret_cast<X87Context*>(actual.state->uniform_buffer_begin);
            X87Seed(context, left, right, 0x037f);
            const auto expected = X87Hardware(command, left, right, 0x037f);
            Check(compiled.fn(actual.state) == HaltReason::CallHost, "x87 both-NaN arithmetic halt");
            const auto& result = context.x87_regs[pop ? 7 : 6];
            Check(result.significand == expected.value.significand && result.sign_exp == expected.value.sign_exp,
                  "x87 both-NaN quiet/signaling/payload/sign arbitration matches hardware");
            Check((context.x87_fsw & 0x7fff) == (expected.status & 0x7fff), "x87 both-NaN invalid flag/C1/TOP matches hardware");
        }
    }
    std::cout << "PASS x87 Binary C1 / pop / signed overflow+underflow / two-NaN hardware arbitration\n";
}

void X87InvalidIR() {
    const auto good = MakeX87Command(X87Action::Binary, X87Format::Register, 1, u8(X87Binary::Add));
    std::vector<u64> rejected{good | (u64{1} << 19), good | (u64{1} << 63), good | (u64{0xff} << 24),
        MakeX87Command(X87Action::Compare, X87Format::Register, 1, 0, X87Pop | X87PopTwice),
        MakeX87Command(X87Action::StoreStatus), u64{0xff}};
    for (u64 action = 0; action <= u64(X87Action::LoadEnvironment); ++action)
        for (u64 format = u64(X87Format::Float32); format <= u64(X87Format::Int64); ++format)
            rejected.push_back(action | (format << 8));
    for (auto command : rejected) {
        bool failed{}; auto block = X87Program(command);
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { failed = true; }
        Check(failed, "x87 rejects memory/invalid/reserved command before code publication");
    }
    for (u32 variant = 0; variant < 5; ++variant) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xd700})}; ir::Assembler as{block.get()};
        auto pointer = as.GetUniformAddress(ir::Imm{u64{0}}).SetType(ValueType::U64);
        auto zero = as.LoadImm(ir::Imm{u64{0}}).SetType(ValueType::U64);
        if (variant < 2) pointer = pointer.SetType(ValueType::U32);
        if (variant == 1) pointer = pointer.SetCastType(ValueType::U64);
        if (variant == 2) zero = zero.SetType(ValueType::V128).SetCastType(ValueType::U64);
        (void)as.X87Op(pointer, ir::Imm{good}, zero).SetType(variant == 3 ? ValueType::U16 : variant == 4 ? ValueType::V128 : ValueType::U64);
        block->SetTerminal(ir::terminal::ReturnToHost{}); bool failed{};
        try { Compiled compiled{block.get()}; } catch (const std::runtime_error&) { failed = true; }
        Check(failed, "x87 rejects invalid input/result types and fake casts");
    }
    std::cout << "PASS register x87 strict command/type validation and all memory-format rejection\n";
}
