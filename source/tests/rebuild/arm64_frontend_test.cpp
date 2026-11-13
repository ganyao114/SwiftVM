#include <bit>
#include <utility>
#include "runtime/arch/arm64/service.h"
#include "runtime/exit.h"
#include "runtime/frontend/arm64/build_ir.h"
#include "tests/support/arm64_guest_fixture.h"

namespace {
namespace front = swift::arm64;
namespace ir = swift::runtime::ir;
using State = swift::runtime::arm64::GuestState;
using namespace swift::tests::arm_guest;
using namespace vixl::aarch64;
using U64 = std::uint64_t;

TEST_CASE("ARM FP binary semantics select x64 without changing other targets",
          "[compile][arm64-frontend]") {
    // Compiler-verified FADD D0,D1,D2; FADD V6.2D,V7.2D,V8.2D;
    // FMUL V9.2D,V10.2D,V11.D[1].
    constexpr std::array<std::uint32_t, 3> words{0x1e622820, 0x4e68d4e6, 0x4fcb9949};
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(words.data()),
                                              sizeof(words)};
    const std::array<front::BlockWindow, 1> windows{{{CodeBase, bytes}}};
    for (const auto target : {0x8664u, 0xa64u, 0x5064u})
        for (const auto mode :
             {ir::TargetMode::Known, ir::TargetMode::Unknown, ir::TargetMode::EarlyOff}) {
            const auto built = front::BuildIR(windows, ir::Scope::Block, {}, {mode, target, 0});
            REQUIRE(built);
            unsigned arm_binary{};
            for (const auto& op : built.program.Operations())
                arm_binary += op.opcode == ir::OpCode::ArmFPBinary;
            CHECK(arm_binary == (target == 0x8664 && mode == ir::TargetMode::Known ? 3u : 0u));
        }
}

void Build(const Code& code, ir::Scope scope = ir::Scope::Block) {
    const std::array<front::BlockWindow, 1> windows{{{CodeBase, code.View()}}};
    for (const auto mode : {ir::TargetMode::Known, ir::TargetMode::Unknown}) {
        ir::TargetInfo target;
        target.mode = mode;
        target.target = 0xa64;
        const auto result = scope == ir::Scope::Function
                                    ? front::BuildFunctionIR(windows, CodeBase, {}, {}, target)
                                    : front::BuildIR(windows, scope, {}, target);
        INFO(static_cast<unsigned>(result.error));
        INFO(static_cast<unsigned>(result.ir_error));
        INFO(result.error_pc);
        REQUIRE(bool(result));
        CHECK(!result.program.Operations().empty());
        for (const auto& operation : result.program.Operations())
            CHECK(operation.type < ir::Type::V8 || operation.type > ir::Type::V64);
    }
}

TEST_CASE("ARM scalar SIMD stores use complete zero-extended Q values on x64",
          "[compile][arm64-frontend]") {
    // LDR B/H/S/D0,[X1]; FMOV S0,W1; FMOV D0,X1; FABS D0,D1.
    for (const std::uint32_t word : {0x3d400020u,
                                     0x7d400020u,
                                     0xbd400020u,
                                     0xfd400020u,
                                     0x1e270020u,
                                     0x9e670020u,
                                     0x1e60c020u}) {
        const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(&word),
                                                  sizeof(word)};
        const std::array<front::BlockWindow, 1> windows{{{CodeBase, bytes}}};
        for (const auto mode : {ir::TargetMode::Known, ir::TargetMode::Unknown}) {
            front::StateLayout layout;
            layout.fprs_offset += 64;
            const auto built = front::BuildIR(windows, ir::Scope::Block, layout, {mode, 0x8664, 0});
            REQUIRE(built);
            unsigned stores{};
            for (unsigned node = 0; node < built.program.Operations().size(); ++node) {
                const auto& op = built.program.Operations()[node];
                if (op.opcode != ir::OpCode::StoreUniform) continue;
                const auto field = built.program.Operands(ir::NodeId{node}).front();
                if (field.Payload() < layout.fprs_offset ||
                    field.Payload() >= layout.fprs_offset + 16)
                    continue;
                ++stores;
                CHECK(field.TypeView() ==
                      (mode == ir::TargetMode::Known ? ir::Type::V128 : ir::Type::U64));
            }
            CHECK(stores == (mode == ir::TargetMode::Known ? 1u : 2u));
        }
    }
}

TEST_CASE("ARM x64 numerical IR declares comparison flags without a data producer",
          "[compile][arm64-frontend]") {
    // FCMP D1,D2; FMADD D0,D1,D2,D3; FSQRT D4,D1.
    constexpr std::uint32_t words[]{0x1e622020, 0x1f420c20, 0x1e61c024};
    const std::span<const std::uint8_t> bytes{reinterpret_cast<const std::uint8_t*>(words),
                                              sizeof(words)};
    const std::array<front::BlockWindow, 1> windows{{{CodeBase, bytes}}};
    const auto built =
            front::BuildIR(windows, ir::Scope::Block, {}, {ir::TargetMode::Known, 0x8664, 0});
    REQUIRE(built);
    unsigned comparisons{}, fused{}, unary{};
    for (const auto& op : built.program.Operations()) {
        comparisons += op.opcode == ir::OpCode::ArmFPCompare;
        fused += op.opcode == ir::OpCode::ArmFPTernary;
        unary += op.opcode == ir::OpCode::ArmFPUnary;
        if (op.opcode == ir::OpCode::ArmFPCompare) {
            CHECK(op.type == ir::Type::Void);
            CHECK(op.Metadata() == ir::OperationMetadata{});
            CHECK(ir::HasAnyEffect(ir::EffectsFor(op.opcode), ir::Effect::WriteFlags));
        }
        CHECK(op.opcode != ir::OpCode::CallLambda);
    }
    CHECK(comparisons == 1);
    CHECK(fused == 1);
    CHECK(unary == 1);
}

template <class T> T Lane(const State& s, unsigned r, unsigned lane = 0) {
    T out;
    std::memcpy(&out, s.q[r] + lane * sizeof(T), sizeof(T));
    return out;
}

template <class T> void Set(State& s, unsigned r, T value, unsigned lane = 0) {
    std::memcpy(s.q[r] + lane * sizeof(T), &value, sizeof(value));
}
}  // namespace

TEST_CASE("ARM guest events precede control effects and survive unknown targets",
          "[compile][arm-frontend]") {
    const Code branch{[](auto& a) { a.bl(0); }};
    const Code indirect{[](auto& a) { a.blr(x3); }};
    const Code indirect_jump{[](auto& a) { a.br(x3); }};
    const Code ret{[](auto& a) { a.ret(); }};
    const Code forward{[](auto& a) { a.b(8); }};
    const Code nop{[](auto& a) { a.nop(); }};
    const Code linear{[](auto& a) {
        for (unsigned i = 0; i != 257; ++i) a.nop();
    }};
    const Code budget_indirect{[](auto& a) {
        for (unsigned i = 0; i != 256; ++i) a.nop();
        a.blr(x3);
    }};
    using Point = std::pair<ir::OpCode, U64>;
    const auto point = [](ir::OpCode op, U64 pc) { return std::vector<Point>{{op, pc}}; };
    for (auto mode : {ir::TargetMode::Known, ir::TargetMode::Unknown, ir::TargetMode::EarlyOff})
        for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
            const auto points = [&](const Code& code, U64 pc) {
                const front::BlockWindow window{pc, code.View()};
                const auto result =
                        front::BuildIR({&window, 1},
                                       scope,
                                       {},
                                       {mode, 0, static_cast<U64>(ir::TargetFeature::GuestEvents)});
                REQUIRE(bool(result));
                std::vector<Point> actual;
                for (const auto& op : result.program.Operations())
                    if (op.opcode == ir::OpCode::GuestSafepoint ||
                        op.opcode == ir::OpCode::GuestDispatchSafepoint)
                        actual.emplace_back(op.opcode,
                                            result.program.Operands(op).front().Payload());
                if (!actual.empty())
                    CHECK(result.program.Operations().front().opcode == actual.front().first ||
                          actual.front().second != pc);
                return actual;
            };
            CHECK(points(branch, CodeBase) == point(ir::OpCode::GuestSafepoint, CodeBase));
            CHECK(points(indirect, CodeBase) ==
                  point(ir::OpCode::GuestDispatchSafepoint, CodeBase));
            CHECK(points(indirect_jump, CodeBase) ==
                  point(ir::OpCode::GuestDispatchSafepoint, CodeBase));
            CHECK(points(ret, CodeBase) == point(ir::OpCode::GuestDispatchSafepoint, CodeBase));
            CHECK(points(forward, CodeBase).empty());
            CHECK(points(nop, CodeBase + 4092) ==
                  point(ir::OpCode::GuestSafepoint, CodeBase + 4092));
            CHECK(points(indirect, CodeBase + 4092) ==
                  point(ir::OpCode::GuestSafepoint, CodeBase + 4092));
            CHECK(points(linear, CodeBase) == point(ir::OpCode::GuestSafepoint, CodeBase + 1024));
            CHECK(points(budget_indirect, CodeBase) ==
                  point(ir::OpCode::GuestSafepoint, CodeBase + 1024));
        }
    front::DirectServices services;
    services.guest_events = [](void*) noexcept { return true; };
    for (const auto* code : {&branch, &indirect, &ret}) {
        State state;
        state.pc = CodeBase;
        state.gprs[30] = 0x12345678;
        const auto decoded = front::DecodeOne(code->View(), CodeBase);
        REQUIRE(bool(decoded));
        const auto stopped =
                front::InterpretDecoded(decoded.instruction, code->View(), state, services);
        CHECK(stopped.kind == front::StepKind::Service);
        CHECK(stopped.service ==
              swift::runtime::ExitValue(swift::runtime::NativeExit::GuestEvents));
        CHECK(state.pc == CodeBase);
        CHECK(state.gprs[30] == 0x12345678);
    }
}

TEST_CASE("ARM frontend preserves SP ZR W writes and canonical no-borrow flags",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.subs(x2, x0, Operand(x1));
        a.adc(x3, xzr, Operand(xzr));
        a.sbc(x4, xzr, Operand(xzr));
        a.movz(xzr, 0x1234);
        a.add(sp, sp, Operand(16));
        a.svc(0);
    }};
    Build(code);
    for (bool equal : {false, true}) {
        State state;
        state.pc = CodeBase;
        state.gprs[0] = equal ? 7 : 0;
        state.gprs[1] = equal ? 7 : 1;
        state.gprs[31] = DataBase + 32;
        Memory memory{code};
        const auto result = Run(state, memory);
        CHECK(result.kind == front::StepKind::Service);
        CHECK(result.service == 1);
        CHECK(state.gprs[2] == (equal ? 0 : UINT64_MAX));
        CHECK(state.gprs[3] == U64(equal));
        CHECK(state.gprs[4] == (equal ? 0 : UINT64_MAX));
        CHECK(state.gprs[31] == DataBase + 48);
        CHECK(bool(state.packed_flags & (U64{1} << 29)) == equal);
        CHECK(bool(state.packed_flags & (U64{1} << 30)) == equal);
    }
    const Code narrow{[](auto& a) {
        a.movz(w0, 0x1234);
        a.svc(0);
    }};
    Build(narrow);
    State state;
    state.pc = CodeBase;
    state.gprs[0] = UINT64_MAX;
    Memory memory{narrow};
    Run(state, memory);
    CHECK(state.gprs[0] == 0x1234);
}

TEST_CASE("ARM conditional compare shares local control in both sinks", "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.ccmp(x0, Operand(2), NCFlag, eq);
        a.svc(0);
    }};
    Build(code);
    for (bool condition : {false, true}) {
        State state;
        state.pc = CodeBase;
        state.gprs[0] = 2;
        state.packed_flags = 1 | (condition ? (U64{1} << 30) : 0);
        Memory memory{code};
        const auto result = Run(state, memory);
        CHECK(result.kind == front::StepKind::Service);
        INFO(condition);
        INFO(state.packed_flags);
        CHECK(bool(state.packed_flags & (U64{1} << 31)) == !condition);
        CHECK(bool(state.packed_flags & (U64{1} << 30)) == condition);
        CHECK(bool(state.packed_flags & (U64{1} << 29)));
    }
}

TEST_CASE("ARM division handles architectural zero and signed overflow",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.udiv(x2, x0, x1);
        a.sdiv(x3, x0, x1);
        a.sdiv(w4, w0, w1);
        a.svc(0);
    }};
    Build(code);
    for (U64 divisor : {U64{0}, UINT64_MAX}) {
        State state;
        state.pc = CodeBase;
        state.gprs[0] = U64{1} << 63;
        state.gprs[1] = divisor;
        Memory memory{code};
        const auto result = Run(state, memory);
        CHECK(result.kind == front::StepKind::Service);
        CHECK(state.gprs[2] == 0);
        CHECK(state.gprs[3] == (divisor ? U64{1} << 63 : 0));
        CHECK(state.gprs[4] == 0);
    }
}

TEST_CASE("ARM memory writeback follows successful exact-width access", "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.str(x0, MemOperand(sp, -8, PreIndex));
        a.ldr(x1, MemOperand(sp, 8, PostIndex));
        a.svc(0);
    }};
    Build(code);
    State state;
    state.pc = CodeBase;
    state.gprs[0] = 0x12345678;
    state.gprs[31] = DataBase + 64;
    state.reservation_valid = 1;
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    CHECK(state.gprs[1] == 0x12345678);
    CHECK(state.gprs[31] == DataBase + 64);
    CHECK(memory.reads == 1 && memory.writes == 1);
    CHECK(state.reservation_valid == 0);
    State fault;
    fault.pc = CodeBase;
    fault.gprs[31] = DataBase;
    fault.reservation_valid = 1;
    Memory other{code};
    const auto result = front::InterpretOne(fault, other.Services());
    CHECK(result.kind == front::StepKind::Fault);
    CHECK(fault.gprs[31] == DataBase);
    CHECK(fault.pc == CodeBase);
    CHECK(other.writes == 0);
    CHECK(fault.reservation_valid == 0);
    const Code lanes{[](auto& a) {
        a.str(b0, MemOperand(x0));
        a.str(h0, MemOperand(x0, 16));
        a.str(s0, MemOperand(x0, 32));
        a.str(d0, MemOperand(x0, 48));
        a.ldr(b1, MemOperand(x0));
        a.ldr(h2, MemOperand(x0, 16));
        a.ldr(s3, MemOperand(x0, 32));
        a.ldp(d4, d5, MemOperand(x0, 48));
        a.svc(0);
    }};
    Build(lanes);
    State narrow;
    narrow.pc = CodeBase;
    narrow.gprs[0] = DataBase;
    std::memset(narrow.q, 0xff, sizeof(narrow.q));
    Set(narrow, 0, UINT64_C(0x123456789abcdef0));
    Memory narrow_memory{lanes};
    CHECK(Run(narrow, narrow_memory).kind == front::StepKind::Service);
    constexpr U64 low[]{0xf0, 0xdef0, 0x9abcdef0, UINT64_C(0x123456789abcdef0), 0};
    for (unsigned r = 1; r <= 5; ++r) {
        CHECK(Lane<U64>(narrow, r) == low[r - 1]);
        CHECK(Lane<U64>(narrow, r, 1) == 0);
    }
}

TEST_CASE("ARM region formation handles conditional edges and direct loop backedges",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.subs(x0, x0, Operand(1));
        a.b(std::int64_t{-1}, ne);
        a.svc(0);
    }};
    Build(code, ir::Scope::Function);
    State state;
    state.pc = CodeBase;
    state.gprs[0] = 4;
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    CHECK(state.gprs[0] == 0);
    const std::array<front::BlockWindow, 1> window{{{CodeBase, code.View()}}};
    std::vector<swift::runtime::SourceSpan> sources;
    const auto result = front::BuildFunctionIR(
            window, CodeBase, {}, {}, {}, nullptr, nullptr, nullptr, {}, &sources);
    REQUIRE(bool(result));
    CHECK(result.program.Blocks().size() == 2);
    CHECK(sources.size() == 2);
    for (const auto mode :
         {ir::TargetMode::Known, ir::TargetMode::Unknown, ir::TargetMode::EarlyOff}) {
        const ir::TargetInfo target{
                mode, 0xa64, std::uint64_t(ir::TargetFeature::ConditionalUniformStore)};
        const auto branch = front::BuildIR(window, ir::Scope::Block, {}, target);
        REQUIRE(bool(branch));
        bool literal_successors{};
        for (const auto& operation : branch.program.Operations())
            literal_successors |= operation.opcode == ir::OpCode::StoreUniformSelectImm;
        CHECK(literal_successors == (mode == ir::TargetMode::Known));
    }
    // A not-taken branch still ends a direct block; it must not consume SVC.
    State fallthrough;
    fallthrough.pc = CodeBase;
    fallthrough.gprs[0] = 1;
    front::DecodeTiming timing;
    const auto block = front::InterpretBlock(fallthrough, memory.Services(), 16, &timing);
    CHECK(block.kind == front::StepKind::Completed);
    CHECK(block.instructions == 2 && block.decoded_bytes == 8);
    CHECK(block.next_pc == CodeBase + 8 && timing.calls == 2);
}

TEST_CASE("ARM NEON multi-register loads wait for all reads before committing Q registers",
          "[compile][arm-frontend]") {
    const Code multiple{
            [](auto& a) { a.ld1(v0.V16B(), v1.V16B(), MemOperand(x0, 32, PostIndex)); }};
    const Code lane{[](auto& a) { a.ld2(v0.V16B(), v1.V16B(), 1, MemOperand(x0, 2, PostIndex)); }};
    const Code replicate{
            [](auto& a) { a.ld2r(v0.V16B(), v1.V16B(), MemOperand(x0, 2, PostIndex)); }};
    const auto second_read_fault = [](const Code& code, std::size_t first_bytes) {
        Build(code);
        Build(code, ir::Scope::Function);
        State state;
        state.pc = CodeBase;
        // The first read succeeds; the second starts beyond the fixture.
        state.gprs[0] = DataBase + 256 - first_bytes;
        std::memset(state.q[0], 0xa5, sizeof(state.q[0]));
        std::memset(state.q[1], 0x5a, sizeof(state.q[1]));
        const auto before = state;
        Memory memory{code};
        std::memset(memory.data.data() + 256 - first_bytes, 0x3c, first_bytes);
        const auto result = front::InterpretOne(state, memory.Services());
        CHECK(result.kind == front::StepKind::Fault);
        CHECK(result.fault_address == DataBase + 256);
        CHECK(memory.reads == 1 && memory.writes == 0);
        CHECK(std::memcmp(state.q, before.q, sizeof(state.q)) == 0);
        CHECK(state.pc == before.pc && state.gprs[0] == before.gprs[0]);
    };
    second_read_fault(multiple, 16);
    second_read_fault(lane, 1);
    second_read_fault(replicate, 1);
}

TEST_CASE("ARM NEON arithmetic and dot preserve lane shape and overlap",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.add(v0.V16B(), v1.V16B(), v2.V16B());
        a.rev64(v3.V16B(), v0.V16B());
        a.sdot(v4.V4S(), v1.V16B(), v2.V16B());
        a.svc(0);
    }};
    Build(code);
    State state;
    state.pc = CodeBase;
    std::memset(state.q[1], 1, 16);
    std::memset(state.q[2], 2, 16);
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(state.q[0][i] == 3);
        CHECK(state.q[3][i] == 3);
    }
    for (unsigned i = 0; i < 4; ++i) CHECK(Lane<std::uint32_t>(state, 4, i) == 8);
    const Code half{[](auto& a) {
        a.add(v0.V8B(), v1.V8B(), v2.V8B());
        a.svc(0);
    }};
    Build(half);
    State half_state;
    half_state.pc = CodeBase;
    std::memset(half_state.q[1], 1, 16);
    std::memset(half_state.q[2], 2, 16);
    Memory half_memory{half};
    CHECK(Run(half_state, half_memory).kind == front::StepKind::Service);
    CHECK(Lane<U64>(half_state, 0) == UINT64_C(0x0303030303030303));
    CHECK(Lane<U64>(half_state, 0, 1) == 0);
}

TEST_CASE("ARM state services end both formations before suffix execution",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.ldaxp(x0, x2, MemOperand(x1));
        a.movz(x3, 99);
        a.svc(0);
    }};
    Build(code);
    const std::array<front::BlockWindow, 1> windows{{{CodeBase, code.View()}}};
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
        std::vector<U64> continuations;
        const auto result = scope == ir::Scope::Block ? front::BuildIR(windows,
                                                                       scope,
                                                                       {},
                                                                       {},
                                                                       nullptr,
                                                                       nullptr,
                                                                       false,
                                                                       nullptr,
                                                                       nullptr,
                                                                       &continuations)
                                                      : front::BuildFunctionIR(windows,
                                                                               CodeBase,
                                                                               {},
                                                                               {},
                                                                               {},
                                                                               nullptr,
                                                                               nullptr,
                                                                               nullptr,
                                                                               {},
                                                                               nullptr,
                                                                               &continuations);
        REQUIRE(bool(result));
        CHECK(result.instruction_count == 1 && result.decoded_bytes == 4);
        CHECK(continuations == std::vector<U64>{CodeBase + 4});
    }
    State state;
    state.pc = CodeBase;
    Memory memory{code};
    const auto step = front::InterpretBlock(state, memory.Services());
    CHECK(step.kind == front::StepKind::Service && step.service == 5);
    CHECK(state.pc == CodeBase && step.instructions == 1 && state.gprs[3] == 0);
    const auto request = swift::runtime::arm64::DecodeStateService(state.service_word);
    CHECK(request.kind == swift::runtime::arm64::StateServiceKind::LoadExclusive);
    CHECK(request.pair && request.ordered && request.TransferBytes() == 16);
    CHECK(request.address_register == 1 && request.first_register == 0 &&
          request.second_register == 2);
}

TEST_CASE("ARM saturating multiply reports sticky QC without old IR extensions",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.sqrdmulh(v0.V8H(), v1.V8H(), v2.V8H());
        a.svc(0);
    }};
    Build(code);
    State state;
    state.pc = CodeBase;
    for (unsigned i = 0; i < 8; ++i) {
        Set(state, 1, std::uint16_t{0x8000}, i);
        Set(state, 2, std::uint16_t{0x8000}, i);
    }
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    for (unsigned i = 0; i < 8; ++i) CHECK(Lane<std::uint16_t>(state, 0, i) == 0x7fff);
    CHECK(state.fpsr & (1u << 27));
}

TEST_CASE("ARM FP helper uses ARM controls NaN payload and independent flags",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.fadd(d0, d1, d2);
        a.fcmp(d1, d2);
        a.svc(0);
    }};
    Build(code);
    State state;
    state.pc = CodeBase;
    Set(state, 1, 1.5);
    Set(state, 2, 2.25);
    Memory memory{code};
    CHECK(Run(state, memory).kind == front::StepKind::Service);
    CHECK(Lane<double>(state, 0) == 3.75);
    CHECK(Lane<U64>(state, 0, 1) == 0);
    CHECK(state.packed_flags & (U64{1} << 31));
    CHECK(!(state.packed_flags & (U64{1} << 29)));
    State nan;
    nan.pc = CodeBase;
    Set(nan, 1, UINT64_C(0x7ff0000000000042));
    Set(nan, 2, 1.0);
    Memory other{code};
    CHECK(front::InterpretOne(nan, other.Services()).kind == front::StepKind::Completed);
    CHECK(Lane<U64>(nan, 0) == UINT64_C(0x7ff8000000000042));
    CHECK(nan.fpsr & 1);
}

TEST_CASE("ARM FCVTXN jams inexact low bits independently of FPCR rounding",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) { a.fcvtxn(v0.V2S(), v1.V2D()); }};
    Build(code);
    for (unsigned rounding = 0; rounding < 4; ++rounding) {
        State state;
        state.pc = CodeBase;
        state.fpcr = rounding << 22;
        state.fpsr = 1u << 27;
        std::memset(state.q[0], 0xff, sizeof(state.q[0]));
        Set(state, 1, UINT64_C(0x3ff0000000000001));
        Set(state, 1, UINT64_C(0xbff0000000000001), 1);
        Memory memory{code};
        REQUIRE(front::InterpretOne(state, memory.Services()).kind == front::StepKind::Completed);
        CHECK(Lane<std::uint32_t>(state, 0) == 0x3f800001);
        CHECK(Lane<std::uint32_t>(state, 0, 1) == 0xbf800001);
        CHECK(Lane<U64>(state, 0, 1) == 0);
        CHECK(state.fpsr == ((1u << 27) | 16));
    }
}

TEST_CASE("ARM system writes stop at FP rebinding and invalid stops do not execute",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) {
        a.msr(FPCR, x0);
        a.svc(0);
    }};
    Build(code);
    State state;
    state.pc = CodeBase;
    state.gprs[0] = 1u << 24;
    Memory memory{code};
    const auto result = front::InterpretOne(state, memory.Services());
    CHECK(result.kind == front::StepKind::Service);
    CHECK(result.service == 3);
    CHECK(state.fpcr == (1u << 24));
    CHECK(state.pc == CodeBase + 4);
    const Code illegal{[](auto& a) { a.hlt(0); }};
    State bad;
    bad.pc = CodeBase;
    Memory other{illegal};
    CHECK(front::InterpretOne(bad, other.Services()).kind == front::StepKind::Unsupported);
    const std::array<front::BlockWindow, 1> w{{{CodeBase, illegal.View()}}};
    CHECK(!front::BuildIR(w, ir::Scope::Block));
    CHECK(!front::DecodeOne(illegal.View(), CodeBase + 1));
    CHECK(!front::DecodeOne(illegal.View().first(3), CodeBase));
}

TEST_CASE("ARM BRK returns an encoded exception at the faulting instruction",
          "[compile][arm-frontend]") {
    const Code code{[](auto& a) { a.brk(17); }};
    const auto service =
            swift::runtime::SemanticExceptionExit(swift::runtime::GuestExceptionKind::Breakpoint);
    const front::BlockWindow window{CodeBase, code.View()};
    for (auto scope : {ir::Scope::Block, ir::Scope::Function}) {
        const auto result = front::BuildIR({&window, 1}, scope);
        REQUIRE(bool(result));
        CHECK(result.instruction_count == 1 && result.decoded_bytes == 4);
        const auto& terminal = result.program.Blocks()[0].terminal;
        CHECK(terminal.kind == ir::TerminalKind::Return && terminal.HasValue());
        const auto& returned = result.program.Operations()[terminal.value.value];
        CHECK(returned.opcode == ir::OpCode::LoadImm);
        CHECK(result.program.Operands(returned).front().Payload() == service);
    }
    State state;
    state.pc = CodeBase;
    state.gprs[0] = 123;
    const auto flags = state.packed_flags;
    Memory memory{code};
    const auto step = front::InterpretOne(state, memory.Services());
    CHECK(step.kind == front::StepKind::Service && step.service == service);
    CHECK(step.pc == CodeBase && step.next_pc == CodeBase && state.pc == CodeBase);
    CHECK(state.gprs[0] == 123 && state.packed_flags == flags);
    // An unallocated encoding remains a decoder error, separate from a valid
    // but unsupported instruction such as HLT, and from BRK's semantic event.
    auto invalid = code;
    invalid.words[0] = 0;
    CHECK(front::DecodeOne(invalid.View(), CodeBase).error == front::DecodeError::InvalidEncoding);
    Memory invalid_memory{invalid};
    CHECK(front::InterpretOne(state, invalid_memory.Services()).kind ==
          front::StepKind::InvalidEncoding);
}
