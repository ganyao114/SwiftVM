#include "tests/support/arm64_fixture.h"

namespace {
using namespace swift::tests::arm64_fixture;

void VectorSignedWidenMultiply() {
    using Vector = std::array<u64, 2>;

    struct Case {
        unsigned bits;
        Vector left, right, expected;
        bool square;
    };

    constexpr std::array cases{
            Case{8,
                 {0x7f80ff010200feffULL, 0x1111222233334444ULL},
                 {0x807f02fffe050103ULL, 0xffff80007fff0001ULL},
                 {0xfffc0000fffefffdULL, 0xc080c080fffeffffULL},
                 false},
            Case{16,
                 {0x00027fff8000ffffULL, 0xabcdef0123456789ULL},
                 {0xfffdfffe8000ffffULL, 0x13579bdf2468ace0ULL},
                 {0x4000000000000001ULL, 0xfffffffaffff0002ULL},
                 false},
            Case{32,
                 {0x7fffffff80000000ULL, 0xdeadbeef01234567ULL},
                 {},
                 {0x4000000000000000ULL, 0x3fffffff00000001ULL},
                 true},
    };
    std::array<std::uint8_t, cases.size() * 32> state{};
    std::array<Value, cases.size()> left{}, products{};
    Builder builder{Scope::Block, arm64::KnownTargetInfo()};
    Need(builder.BeginBlock(Create(builder)));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto offset = static_cast<std::uint32_t>(i * 32);
        std::memcpy(state.data() + offset, cases[i].left.data(), sizeof(Vector));
        std::memcpy(state.data() + offset + 16, cases[i].right.data(), sizeof(Vector));
        left[i] = Need(builder.Emit<OpCode::LoadUniform>(Type::V128,
                                                         {Operand::Uniform(offset, Type::V128)}));
        const auto right =
                cases[i].square ? left[i]
                                : Need(builder.Emit<OpCode::LoadUniform>(
                                          Type::V128, {Operand::Uniform(offset + 16, Type::V128)}));
        products[i] = Need(builder.Emit<OpCode::VecSignedMulWidenLow>(
                Type::V128,
                {Operand::ValueOf(left[i]),
                 Operand::ValueOf(right),
                 Operand::Immediate(cases[i].bits, Type::U8)}));
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(products[i])}));
    }
    Need(builder.EndBlock(Terminal::Return(Constant(builder, 0))));
    auto program = Submit(builder);
    Check(program.Target().Has(TargetFeature::VectorSignedMulWidenLow),
          "known backend advertises the neutral signed low-half widening product");
    const auto code = ::Compile(program);
    Check(static_cast<std::size_t>(std::count_if(code.words.begin(),
                                                 code.words.end(),
                                                 [](std::uint32_t word) {
                                                     return (word & 0xff20fc00u) == 0x0e20c000u;
                                                 })) == cases.size(),
          "each supported lane width selects one complete SMULL result");

    arm64::CompileOptions pinned;
    pinned.pinned_vector_offset = 0;
    pinned.private_uniforms = true;
    const auto allocation = arm64::AllocateBlock(program, &pinned);
    Check(bool(allocation), "signed widening product accepts the existing vector pin allocation");
    for (std::size_t i = 0; i < cases.size(); ++i)
        Check(!allocation.homes[left[i].node.value].spilled &&
                      allocation.homes[left[i].node.value].reg ==
                              allocation.homes[products[i].node.value].reg,
              "low-half inputs are read before their shared full-result pin is overwritten");
    if (NativeMode()) Execute(code, state.data(), 0);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        for (std::size_t i = 0; i < cases.size(); ++i)
            Check(Read64(state.data() + i * 32) == cases[i].expected[0] &&
                          Read64(state.data() + i * 32 + 8) == cases[i].expected[1],
                  "signed low lanes widen exactly, ignore upper noise, and retain the complete "
                  "product");
    }
#endif
}

void PackedFloatingArithmetic() {
    using Vector = std::array<u64, 2>;
    const auto singles = [](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
        return Vector{u64{a} | (u64{b} << 32), u64{c} | (u64{d} << 32)};
    };

    struct State {
        u64 flags;
        std::uint32_t mxcsr, padding{};
        std::array<Vector, 4> inputs;
        std::array<Vector, 8> outputs{};
        std::array<Vector, 4> denormals;
        std::array<Vector, 4> flushed{};
        std::array<Vector, 2> comparison_inputs;
        std::array<Vector, 5> comparison_outputs{};
        std::array<Vector, 2> pack_inputs;
        Vector minimum{};
    } state{};

    constexpr u64 initial = 0xa5a50000bc00003cULL;
    state.flags = initial;
    state.mxcsr = 0x1f81;  // Raw sticky bit is software-owned and preserved.
    state.inputs = {singles(0x3fc00000, 0xc0000000, 0, 0x41000000),
                    singles(0x3f000000, 0x40800000, 0x40000000, 0x40000000),
                    Vector{0x4010000000000000ULL, 0xc020000000000000ULL},
                    Vector{0x4000000000000000ULL, 0x3fe0000000000000ULL}};
    [[maybe_unused]] const std::array expected{
            singles(0x40000000, 0x40000000, 0x40000000, 0x41200000),
            singles(0x3f800000, 0xc0c00000, 0xc0000000, 0x40c00000),
            singles(0x3f400000, 0xc1000000, 0, 0x41800000),
            singles(0x40400000, 0xbf000000, 0, 0x40800000),
            Vector{0x4018000000000000ULL, 0xc01e000000000000ULL},
            Vector{0x4000000000000000ULL, 0xc021000000000000ULL},
            Vector{0x4020000000000000ULL, 0xc010000000000000ULL},
            Vector{0x4000000000000000ULL, 0xc030000000000000ULL},
    };
    state.denormals = {singles(1, 0x80000001, 0x00800000, 0x80800000),
                       singles(0x7e800000, 0x7e800000, 0x3f000000, 0x3f000000),
                       Vector{1, 0x8010000000000000ULL},
                       Vector{0x7fd0000000000000ULL, 0x3fe0000000000000ULL}};
    [[maybe_unused]] const std::array flush_expected{
            singles(0, 0x80000000, 0x00400000, 0x80400000),
            Vector{0, 0x8008000000000000ULL},
            singles(0x34000000, 0xb4000000, 0, 0x80000000),
            Vector{0x3cb0000000000000ULL, 0x8000000000000000ULL},
    };
    state.comparison_inputs = {singles(0xbf800000, 0, 0x40000000, 0x7fc01234),
                               singles(0, 0x80000000, 0x3f800000, 0x3f800000)};
    [[maybe_unused]] const std::array comparison_expected{
            singles(0xffffffff, 0, 0, 0),
            singles(0, 0xffffffff, 0, 0),
            singles(0xffffffff, 0, 0xffffffff, 0xffffffff),
            singles(0xffffffff, 0xffffffff, 0xffffffff, 0),
            singles(0xffffffff, 0xffffffff, 0, 0),
    };
    state.pack_inputs = {Vector{0x00010000ffff8000ULL, 0x7fff010000ff00feULL},
                         Vector{0x0080007f0002fffeULL, 0x0400fed4012c00ffULL}};
    const std::array ops{OpCode::VecFAdd, OpCode::VecFSub, OpCode::VecFMul, OpCode::VecFDiv};
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto load_vector = [&](std::uint32_t offset) {
        return Need(builder.Emit<OpCode::LoadUniform>(Type::V128,
                                                      {Operand::Uniform(offset, Type::V128)}));
    };
    const auto store_vector = [&](std::uint32_t offset, Value value) {
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void, {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value)}));
    };
    for (unsigned type = 0; type < 2; ++type) {
        const auto left = load_vector(offsetof(State, inputs) + type * 32);
        const auto right = load_vector(offsetof(State, inputs) + type * 32 + 16);
        for (unsigned op = 0; op < ops.size(); ++op) {
            const auto result =
                    Need(builder.Append(ops[op],
                                        Type::V128,
                                        {Operand::ValueOf(left),
                                         Operand::ValueOf(right),
                                         Operand::Immediate(type ? 64 : 32, Type::U8)}));
            store_vector(offsetof(State, outputs) + (type * 4 + op) * 16, result);
        }
    }
    const auto compare_left = load_vector(offsetof(State, comparison_inputs));
    const auto compare_right = load_vector(offsetof(State, comparison_inputs) + 16);
    constexpr std::array relations{1u, 2u, 13u, 7u};  // LT, EQ, NEQ_UQ and ORD.
    for (unsigned i = 0; i < relations.size(); ++i)
        store_vector(
                offsetof(State, comparison_outputs) + i * 16,
                Need(builder.Emit<OpCode::VecFCmpMask>(Type::V128,
                                                       {Operand::ValueOf(compare_left),
                                                        Operand::ValueOf(compare_right),
                                                        Operand::Immediate(32, Type::U8),
                                                        Operand::Immediate(relations[i], Type::U8),
                                                        Operand::Immediate(0, Type::U8)})));
    const auto denormal32 = load_vector(offsetof(State, denormals)),
               factor32 = load_vector(offsetof(State, denormals) + 16);
    const auto denormal64 = load_vector(offsetof(State, denormals) + 32),
               factor64 = load_vector(offsetof(State, denormals) + 48);
    const auto zeros = Need(builder.Emit<OpCode::VecSharedZero>(Type::V128, {}));
    for (unsigned output_flush = 0; output_flush < 2; ++output_flush) {
        Need(builder.Emit<OpCode::StoreUniform>(
                Type::Void,
                {Operand::Uniform(offsetof(State, mxcsr), Type::U32),
                 Operand::ValueOf(Constant(builder, output_flush ? 0x9f81 : 0x1fc1), Type::U32)}));
        store_vector(offsetof(State, flushed) + output_flush * 32,
                     Need(builder.Emit<OpCode::VecFMul>(Type::V128,
                                                        {Operand::ValueOf(denormal32),
                                                         Operand::ValueOf(factor32),
                                                         Operand::Immediate(32, Type::U8)})));
        store_vector(offsetof(State, flushed) + output_flush * 32 + 16,
                     Need(builder.Emit<OpCode::VecFMul>(Type::V128,
                                                        {Operand::ValueOf(denormal64),
                                                         Operand::ValueOf(factor64),
                                                         Operand::Immediate(64, Type::U8)})));
        if (!output_flush)
            store_vector(
                    offsetof(State, comparison_outputs) + 4 * 16,
                    Need(builder.Emit<OpCode::VecFCmpMask>(Type::V128,
                                                           {Operand::ValueOf(denormal32),
                                                            Operand::ValueOf(zeros),
                                                            Operand::Immediate(32, Type::U8),
                                                            Operand::Immediate(2, Type::U8),
                                                            Operand::Immediate(0, Type::U8)})));
    }
    const auto pack_left = load_vector(offsetof(State, pack_inputs));
    const auto pack_right = load_vector(offsetof(State, pack_inputs) + 16);
    store_vector(offsetof(State, pack_inputs) + 16,
                 Need(builder.Emit<OpCode::VecPack>(Type::V128,
                                                    {Operand::ValueOf(pack_left),
                                                     Operand::ValueOf(pack_right),
                                                     Operand::Immediate(16, Type::U8),
                                                     Operand::Immediate(1, Type::U8)})));
    const auto minimum_left = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0x0080ff1020304050ULL, Type::U64),
             Operand::Immediate(0xff80017f00fe0203ULL, Type::U64)}));
    const auto minimum_right = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0x0181fe2020108060ULL, Type::U64),
             Operand::Immediate(0x7fff027f01ff0104ULL, Type::U64)}));
    store_vector(offsetof(State, minimum),
                 Need(builder.Emit<OpCode::VecMin>(Type::V128,
                                                   {Operand::ValueOf(minimum_left),
                                                    Operand::ValueOf(minimum_right),
                                                    Operand::Immediate(8, Type::U8),
                                                    Operand::Immediate(0, Type::U8)})));
    Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
    auto program = Submit(builder);
    arm64::CompileOptions options{offsetof(State, flags)};
    options.mxcsr_flush_offset = offsetof(State, mxcsr);
    const auto code = ::Compile(program, options);
    for (const auto opcode : {0x4e20d400u,
                              0x4ea0d400u,
                              0x6e20dc00u,
                              0x6e20fc00u,
                              0x4e60d400u,
                              0x4ee0d400u,
                              0x6e60dc00u,
                              0x6e60fc00u})
        Check(std::any_of(code.words.begin(),
                          code.words.end(),
                          [&](std::uint32_t word) { return (word & 0xffe0fc00u) == opcode; }),
              "packed IEEE arithmetic uses the actual 4S/2D operation");
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) { return (word & 0xfffffc00u) == 0x2e212800u; }) &&
                  std::any_of(
                          code.words.begin(),
                          code.words.end(),
                          [](std::uint32_t word) { return (word & 0xfffffc00u) == 0x6e212800u; }),
          "signed16 to unsigned8 packing uses saturating SQXTUN and SQXTUN2");
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        // Standalone compiled units borrow their caller's FP environment. Set
        // it explicitly; the separate runtime Root owns its guest/host switches.
        struct RestoreFP {
            u64 fpcr{}, fpsr{};

            ~RestoreFP() {
                asm volatile("msr fpcr,%0\nmsr fpsr,%1" ::"r"(fpcr), "r"(fpsr) : "memory");
            }
        } restore;

        asm volatile("mrs %0,fpcr\nmrs %1,fpsr" : "=r"(restore.fpcr), "=r"(restore.fpsr));
        asm volatile("msr fpcr,xzr\nmsr fpsr,xzr" ::: "memory");
    }
#endif
    if (NativeMode()) Execute(code, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(state.outputs == expected && state.flushed == flush_expected,
              "packed arithmetic computes every lane and independently applies DAZ inputs or FTZ "
              "outputs with signed zeros");
        Check(state.comparison_outputs == comparison_expected,
              "four-lane IEEE compare masks distinguish LT/EQ/GT/unordered and apply the actual "
              "DAZ "
              "control");
        Check(state.pack_inputs[0] == Vector{0x00010000ffff8000ULL, 0x7fff010000ff00feULL} &&
                      state.pack_inputs[1] == Vector{0xfffffffe01000000ULL, 0xff00ffff807f0200ULL},
              "packing clamps signed negatives and overflow to 0..255 while publishing over the "
              "original right field");
        Check(state.minimum == Vector{0x0080fe1020104050ULL, 0x7f80017f00fe0103ULL},
              "unsigned byte minimum compares all sixteen complete lanes including values above "
              "signed-byte range");
        Check(state.flags == initial && state.mxcsr == 0x9f81,
              "packed arithmetic preserves whole guest flags and the raw software-owned MXCSR "
              "status/control word");
    }
#endif
    // This separate finite-only unit never changes its software flush mode.
    // The preceding mixed-control unit deliberately retains the unknown API.
    Builder finite{Scope::Block};
    Need(finite.BeginBlock(Create(finite)));
    for (unsigned type = 0; type < 2; ++type) {
        const auto left = Need(finite.Emit<OpCode::LoadUniform>(
                Type::V128, {Operand::Uniform(offsetof(State, inputs) + type * 32, Type::V128)}));
        const auto right = Need(finite.Emit<OpCode::LoadUniform>(
                Type::V128,
                {Operand::Uniform(offsetof(State, inputs) + type * 32 + 16, Type::V128)}));
        for (unsigned op = 0; op < ops.size(); ++op) {
            const auto result = Need(finite.Append(ops[op],
                                                   Type::V128,
                                                   {Operand::ValueOf(left),
                                                    Operand::ValueOf(right),
                                                    Operand::Immediate(type ? 64 : 32, Type::U8)}));
            Need(finite.Emit<OpCode::StoreUniform>(
                    Type::Void,
                    {Operand::Uniform(offsetof(State, outputs) + (type * 4 + op) * 16, Type::V128),
                     Operand::ValueOf(result)}));
        }
    }
    Need(finite.EndBlock(Terminal::Return(Constant(finite, 42))));
    auto finite_program = Submit(finite);
    auto known_options = options;
    known_options.mxcsr_flush_mode = 0;
    const auto known_default = ::Compile(finite_program, known_options);
    auto direct_options = options;
    direct_options.mxcsr_flush_offset = UINT32_MAX;
    const auto direct = ::Compile(finite_program, direct_options);
    Check(known_default.words == direct.words,
          "known default software controls emit exactly the direct packed arithmetic path without "
          "copies, loads or control branches");
    state.mxcsr = 0x1f81;
    state.outputs = {};
    if (NativeMode()) Execute(known_default, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(state.outputs == expected && state.flags == initial && state.mxcsr == 0x1f81,
              "known default packed arithmetic preserves every finite lane, whole flags and raw "
              "status");
    }
#endif
    state.inputs = state.denormals;
    for (const std::uint8_t mode : {std::uint8_t{1}, std::uint8_t{2}}) {
        known_options.mxcsr_flush_mode = mode;
        const auto known = ::Compile(finite_program, known_options);
        state.mxcsr = mode == 1 ? 0x1fc1 : 0x9f81;
        state.outputs = {};
        if (NativeMode()) Execute(known, reinterpret_cast<std::uint8_t*>(&state), 42);
#if defined(__aarch64__) && defined(__ANDROID__)
        if (NativeMode()) {
            const auto output = mode == 1 ? 0u : 2u;
            Check(state.outputs[2] == flush_expected[output] &&
                          state.outputs[6] == flush_expected[output + 1] &&
                          state.flags == initial && state.mxcsr == (mode == 1 ? 0x1fc1u : 0x9f81u),
                  "known DAZ-only and FTZ-only modes retain their independent signed subnormal "
                  "semantics without raw control reads");
        }
#endif
    }
}

void PackedFPStatus() {
    using Vector = std::array<u64, 2>;

    struct State {
        Vector left, right, result{};
        u64 flags{};
        std::uint32_t control{}, status{};
        Vector kept_left{}, kept_right{};
    };

    struct Case {
        OpCode opcode;
        unsigned relation;
        Vector left, right, expected;
        std::uint32_t status;
        std::uint8_t mode{};
        bool scalar{};
    };

    [[maybe_unused]] constexpr u64 initial_flags = 0xa5a50000bc00003cULL;
    for (const unsigned bits : {32u, 64u}) {
        const u64 one = bits == 32 ? 0x3f800000 : 0x3ff0000000000000,
                  two = bits == 32 ? 0x40000000 : 0x4000000000000000,
                  half = bits == 32 ? 0x3f000000 : 0x3fe0000000000000,
                  minimum = bits == 32 ? 0x00800000 : 0x0010000000000000,
                  infinity = bits == 32 ? 0x7f800000 : 0x7ff0000000000000,
                  qnan = infinity | (u64{1} << (bits == 32 ? 22 : 51)) | 1, snan = infinity | 1,
                  sign = u64{1} << (bits - 1), mask = bits == 32 ? UINT32_MAX : UINT64_MAX;
        const auto pack = [&](u64 low, u64 high, u64 tail) {
            return bits == 64 ? Vector{low, high} : Vector{low | (high << 32), tail | (tail << 32)};
        };
        const auto compare = [&](unsigned relation, u64 nan, std::uint32_t status) {
            // Lane 0 raises DE; lane 1 independently classifies a NaN. Other
            // lanes are equal normals, so their expected mask is independent.
            return Case{OpCode::VecFCmpMask,
                        relation,
                        pack(1, nan, one),
                        pack(0, one, one),
                        pack((relation & 4) ? mask : 0,
                             (relation & 8) ? mask : 0,
                             (relation & 2) ? mask : 0),
                        status};
        };
        const std::array cases{
                compare(17, qnan, 3),  // Native signaling LT.
                compare(1, qnan, 2),   // Quiet LT needs signaling-host correction.
                compare(2, qnan, 2),   // Native quiet EQ.
                compare(2, snan, 3),
                compare(18, qnan, 3),  // Signaling EQ needs quiet-host correction.
                compare(8, qnan, 2),   // Quiet unordered uses a composite predicate.
                compare(24, qnan, 3),
                compare(0, qnan, 2),  // Constant false still observes exception inputs.
                compare(16, qnan, 3),
                compare(15, snan, 3),  // Constant true must observe sNaN even when quiet.
                compare(31, qnan, 3),
                Case{OpCode::VecFCmpMask,
                     17,
                     pack(one, snan, 1),
                     pack(two, qnan, qnan),
                     pack(mask, snan, 1),
                     0,
                     0,
                     true},
                // NaN suppresses same-lane DE, while another lane may still set it.
                Case{OpCode::VecFCmpMask,
                     17,
                     pack(1, 1, one),
                     pack(qnan, 0, one),
                     pack(0, 0, 0),
                     3},
                // Zero-divide has priority over DE only in its own lane.
                Case{OpCode::VecFDiv,
                     0,
                     pack(1, 1, one),
                     pack(0, one, one),
                     pack(infinity, 1, one),
                     6},
                Case{OpCode::VecFMul,
                     0,
                     pack(1, sign | 1, one),
                     pack(one, one, one),
                     pack(0, sign, one),
                     0,
                     1},
                Case{OpCode::VecFMul,
                     0,
                     pack(minimum, sign | minimum, one),
                     pack(half, half, one),
                     pack(0, sign, one),
                     48,
                     2},
                Case{OpCode::VecFMul,
                     0,
                     pack(1, sign | 1, one),
                     pack(one, one, one),
                     pack(0, sign, one),
                     0,
                     3},
        };
        for (const auto& test : cases) {
            Builder builder{Scope::Block};
            Need(builder.BeginBlock(Create(builder)));
            const auto load = [&](std::uint32_t offset) {
                return Need(builder.Emit<OpCode::LoadUniform>(
                        Type::V128, {Operand::Uniform(offset, Type::V128)}));
            };
            const auto store = [&](std::uint32_t offset, Value value) {
                Need(builder.Emit<OpCode::StoreUniform>(
                        Type::Void,
                        {Operand::Uniform(offset, Type::V128), Operand::ValueOf(value)}));
            };
            const auto left = load(offsetof(State, left)), right = load(offsetof(State, right));
            const auto result =
                    test.opcode == OpCode::VecFCmpMask
                            ? Need(builder.Emit<OpCode::VecFCmpMask>(
                                      Type::V128,
                                      {Operand::ValueOf(left),
                                       Operand::ValueOf(right),
                                       Operand::Immediate(bits, Type::U8),
                                       Operand::Immediate(test.relation, Type::U8),
                                       Operand::Immediate(test.scalar, Type::U8)}))
                            : Need(builder.Append(test.opcode,
                                                  Type::V128,
                                                  {Operand::ValueOf(left),
                                                   Operand::ValueOf(right),
                                                   Operand::Immediate(bits, Type::U8)}));
            store(offsetof(State, result), result);
            store(offsetof(State, kept_left), left);
            store(offsetof(State, kept_right), right);
            const auto shadow = Need(builder.Emit<OpCode::LoadUniform>(
                    Type::U32, {Operand::Uniform(offsetof(State, control), Type::U32)}));
            const auto status =
                    Need(builder.Emit<OpCode::ReadFPStatus>(Type::U32, {Operand::ValueOf(shadow)}));
            Need(builder.Emit<OpCode::StoreUniform>(
                    Type::Void,
                    {Operand::Uniform(offsetof(State, status), Type::U32),
                     Operand::ValueOf(status)}));
            Need(builder.EndBlock(Terminal::Return(Constant(builder, 42))));
            const auto program = Submit(builder);
            // Known software control, dynamically read control, and AFP's FIZ.
            for (unsigned profile = 0; profile != 3; ++profile) {
                arm64::CompileOptions options{offsetof(State, flags)};
                options.mxcsr_flush_offset = profile == 2 ? UINT32_MAX : offsetof(State, control);
                options.mxcsr_flush_mode = profile == 0 ? test.mode : UINT8_MAX;
                options.native_fp_environment = profile == 2;
                const auto code = ::Compile(program, options);
#if defined(__aarch64__) && defined(__ANDROID__)
                if (!NativeMode() || (profile == 2 && !TestBackendOps().capabilities.afp)) continue;
                State state{test.left, test.right};
                state.flags = initial_flags;
                state.control = 0x1fa0u | ((test.mode & 1u) << 6) | ((test.mode & 2u) << 14);
                swift::tests::NativeCode native{code.words, swift::tests::NativeISA::Arm64Android};

                struct RestoreFP {
                    u64 control, status;

                    RestoreFP(u64 next) {
                        asm volatile("mrs %0,fpcr\nmrs %1,fpsr" : "=r"(control), "=r"(status));
                        const u64 previous_overflow = 4;
                        asm volatile("msr fpcr,%0\nmsr fpsr,%1" ::"r"(next), "r"(previous_overflow)
                                     : "memory");
                    }

                    ~RestoreFP() {
                        asm volatile("msr fpcr,%0\nmsr fpsr,%1" ::"r"(control), "r"(status)
                                     : "memory");
                    }
                };

                u64 returned;
                {
                    const u64 fpcr = profile == 2
                                             ? 6u | (test.mode & 1u) | ((u64{test.mode} & 2u) << 23)
                                             : 0;
                    RestoreFP restore{fpcr};
                    returned = native.Run(reinterpret_cast<std::uint8_t*>(&state));
                }
                INFO("bits=" << bits << " predicate=" << test.relation
                             << " mode=" << unsigned(test.mode) << " profile=" << profile);
                Check(returned == 42 && state.result == test.expected &&
                              state.kept_left == test.left && state.kept_right == test.right,
                      "packed/status consumers preserve input homes, scalar upper lanes and "
                      "numeric results");
                Check(state.status == (test.status | 40u) && state.flags == initial_flags,
                      "native FP status preserves prior sticky and shadow while respecting DE/NaN "
                      "precedence");
#endif
            }
        }
    }
}

void VectorLaneMasks() {
    std::array<std::uint8_t, 128> state{};
    Builder builder{Scope::Block};
    Need(builder.BeginBlock(Create(builder)));
    const auto value = Need(builder.Emit<OpCode::VecLoadConst>(
            Type::V128,
            {Operand::Immediate(0xaa5500ff01817f80ULL, Type::U64),
             Operand::Immediate(0x01ff8055007f8001ULL, Type::U64)}));
    const std::array<unsigned, 4> lanes{8, 16, 32, 64};
    Value last;
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        last = Need(builder.Emit<OpCode::VecMovMask>(
                i & 1 ? Type::U64 : Type::U32,
                {Operand::ValueOf(value), Operand::Immediate(lanes[i], Type::U8)}));
        Store(builder, static_cast<std::uint32_t>(i * 8), last);
    }
    const auto memory_base = Constant(builder, reinterpret_cast<std::uintptr_t>(state.data() + 80));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x4450, Type::U64)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                              {Operand::Expression(OperandOp::Add, 2),
                                               Operand::ValueOf(memory_base),
                                               Operand::Immediate(16, Type::U64),
                                               Operand::ValueOf(value)}));
    Need(builder.Emit<OpCode::FaultLocation>(Type::Void, {Operand::Immediate(0x4460, Type::U64)}));
    Need(builder.Emit<OpCode::StoreMemoryTSO>(Type::Void,
                                              {Operand::Expression(OperandOp::Sub, 2),
                                               Operand::ValueOf(memory_base),
                                               Operand::Immediate(16, Type::U64),
                                               Operand::ValueOf(value)}));
    Store(builder, 32, Snapshot(builder, last));
    const auto carry =
            Need(builder.Emit<OpCode::TestFlags>(Type::Bool, {Operand::Flags(Flag::Carry)}));
    Need(builder.EndBlock(Terminal::Return(carry)));
    auto program = Submit(builder);
    arm64::CompileOptions options{48};
    options.fault_locations = true;
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        options.native_nzcv = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
    }
#else
    options.native_nzcv = true;
#endif
    const auto code = ::Compile(program, options);
    Check(std::any_of(code.words.begin(),
                      code.words.end(),
                      [](std::uint32_t word) {
                          return (word & 0xffc00000u) == 0x3d800000u && ((word >> 10) & 4095u) == 1;
                      }) &&
                  code.faults.size() == 2 &&
                  std::all_of(code.faults.begin(),
                              code.faults.end(),
                              [](const arm64::FaultSite& site) {
                                  return WordOffset(site.unaligned_offset) == UINT32_MAX;
                              }),
          "V128 TSO store uses STR Q scaled16 without RCpc and retains ordinary fault sites");
    constexpr u64 initial = 0xa5a50000bc00003cULL;
    Write64(state.data() + 48, initial);
    if (NativeMode()) Execute(code, state.data(), 1);
#if defined(__aarch64__) && defined(__ANDROID__)
    if (NativeMode()) {
        Check(Read64(state.data()) == 0x6295 && Read64(state.data() + 8) == 0x58 &&
                      Read64(state.data() + 16) == 2 && Read64(state.data() + 24) == 1,
              "lane masks extract arbitrary input sign bits in low-lane order and clear high bits");
        Check(Read64(state.data() + 32) == initial && Read64(state.data() + 48) == initial,
              "lane masks preserve guest flags");
        Check(Read64(state.data() + 64) == 0xaa5500ff01817f80ULL &&
                      Read64(state.data() + 72) == 0x01ff8055007f8001ULL &&
                      Read64(state.data() + 96) == 0xaa5500ff01817f80ULL &&
                      Read64(state.data() + 104) == 0x01ff8055007f8001ULL &&
                      Read64(state.data() + 80) == 0 && Read64(state.data() + 88) == 0,
              "positive vector offset and negative full-EA fallback preserve the original base and "
              "all "
              "data bits");
    }
#endif
}

}  // namespace

TEST_CASE("VectorLaneMasks", "[compile][native][arm64]") { CHECK_NOTHROW(VectorLaneMasks()); }

TEST_CASE("VectorSignedWidenMultiply", "[compile][native][arm64]") {
    CHECK_NOTHROW(VectorSignedWidenMultiply());
}

TEST_CASE("PackedFloatingArithmetic", "[compile][native][arm64]") {
    CHECK_NOTHROW(PackedFloatingArithmetic());
}

TEST_CASE("PackedFPStatus", "[compile][native][arm64]") { CHECK_NOTHROW(PackedFPStatus()); }
