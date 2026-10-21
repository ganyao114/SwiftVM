#include "test_support.h"

#include <bit>
#include <sys/resource.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::tests::riscv;
namespace be = swift::runtime::backend;
using T = ir::ValueType;
using O = ir::OpCode;

namespace {
u64 Helper() { return 7; }

unsigned SaveCount(ir::Block* block) {
    unsigned count{};
    for (const auto& inst : block->GetInstList()) count += inst.GetOp() == O::SaveFlags;
    return count;
}

IntrusivePtr<ir::Block> Program(u64 guest, T type, ir::Flags mask, ir::Cond condition,
                              const std::string& boundary = {}) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{guest})};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto right = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    auto dead = as.Add(left, ir::Operand{right}).SetType(type);
    as.SaveFlags(dead, ir::Flags::All);
    auto dead_again = as.Add(left, ir::Operand{ir::Imm{u64{3}}}).SetType(type);
    as.SaveFlags(dead_again, ir::Flags::All);
    // Any inserted boundary must prevent elimination even of earlier writes
    // that would be dead in the corresponding straight-line integer unit.
    if (boundary == "memory") {
        auto value = as.LoadMemory(ir::Operand{left}).SetType(type);
        as.StoreUniform(ir::Uniform{112, type}, value);
    } else if (boundary == "helper") {
        auto value = ir::Value{block->AppendInst(O::CallLambda,
                ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}})}.SetType(T::U64);
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "label") {
        auto edge = as.Goto(ir::BOOL{as.TestNotZero(left).SetType(T::U8)});
        as.BindLabel(edge);
    } else if (boundary == "div" || boundary == "adc" || boundary == "sbb") {
        const auto op = boundary == "div" ? O::Div : boundary == "adc" ? O::Adc : O::Sbb;
        auto value = ir::Value{block->AppendInst(op, left, ir::Operand{right})}.SetType(type);
        as.StoreUniform(ir::Uniform{112, type}, value);
    } else if (boundary == "fault") {
        block->AppendInst(O::CheckMemoryAlignment, left, ir::Imm{u64{8}});
    } else if (boundary == "barrier") {
        block->AppendInst(O::MemoryBarrierTSO);
    } else if (boundary == "vector") {
        auto value = as.LoadUniform(ir::Uniform{128, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{144, T::V128}, value);
    } else if (boundary == "uniform-range") {
        auto value = as.LoadUniform(ir::Uniform{4095, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "uniform-load-width") {
        auto value = as.LoadUniform(ir::Uniform{4095, T::U8}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "uniform-store-width") {
        as.StoreUniform(ir::Uniform{4095, T::U64}, left);
        // Imported IR can carry inconsistent declarations. Admission must
        // use the actual native store width; do not execute this bad access.
        block->GetInstList().back().SetArg(0, ir::Uniform{4095, T::U8});
    } else if (boundary == "undefined-logical-flags" || boundary == "undefined-mul-flags") {
        const auto op = boundary == "undefined-logical-flags" ? O::Xor : O::Mul;
        auto value = ir::Value{block->AppendInst(op, left, ir::Operand{right})}.SetType(type);
        as.SaveFlags(value, ir::Flags::All);
    }
    auto sum = as.Add(left, ir::Operand{right}).SetType(type);
    as.SaveFlags(sum, ir::Flags::All);
    as.StoreUniform(ir::Uniform{24, T::U8}, as.TestFlags(mask));
    // GetFlags returns the complete word even when its IR argument is narrow.
    as.StoreUniform(ir::Uniform{32, T::U64}, as.GetFlags(sum, mask).SetType(T::U64));
    auto decrement = as.Sub(sum, ir::Operand{ir::Imm{u64{1}}}).SetType(type);
    as.SaveFlags(decrement, ir::Flags::All & ~ir::Flags::Carry);
    as.StoreUniform(ir::Uniform{40, T::U64}, as.GetFlags(decrement, ir::Flags::All).SetType(T::U64));
    as.StoreUniform(ir::Uniform{48, T::U8}, as.TestFlags(mask));
    as.StoreUniform(ir::Uniform{56, T::U8}, as.TestNotFlags(mask));
    as.StoreUniform(ir::Uniform{64, T::U8}, as.CondSet(condition).SetType(T::U8));
    as.StoreUniform(ir::Uniform{72, T::U8}, as.LocalCondSet(condition).SetType(T::U8));
    as.StoreUniform(ir::Uniform{80, type}, as.CondSelect(condition, left, right).SetType(type));
    as.StoreUniform(ir::Uniform{88, type}, decrement);
    if (boundary == "poll") block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
    else block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

struct Environment {
    Config config{TestConfig()};
    be::AddressSpace space{config};
    std::shared_ptr<be::Module> module;
    explicit Environment(Optimizations opts) {
        module = space.MapModule(0x10000, 0x20000,
                                 {.read_only = true, .optimizations = opts});
    }
};

void PositiveReferences(Environment& original, Environment& optimized) {
    u64 seed = 0xba5afcf0836174c9ULL;
    unsigned programs{};
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 low_mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u32 subset = 0; subset < 64; ++subset) {
            const auto mask = static_cast<ir::Flags>((subset & 31) | ((subset & 32) << 1));
            const auto cond = static_cast<ir::Cond>(subset % 16);
            const auto guest = 0x10000 + 16 * programs++;
            auto reference_ir = Program(guest, type, mask, cond);
            auto candidate_ir = Program(guest, type, mask, cond);
            if (programs == 1) std::cout << "first reference program prepared\n";
            auto reference = reinterpret_cast<BlockFn>(rv::CompileBlock(original.module, reference_ir.get()));
            if (programs == 1) std::cout << "first original program compiled\n";
            auto candidate = reinterpret_cast<BlockFn>(rv::CompileBlock(optimized.module, candidate_ir.get()));
            if (programs == 1) std::cout << "first candidate program compiled\n";
            Check(reference && candidate, "compile both pure scalar reference variants");
            Check(SaveCount(reference_ir.get()) == 4 && SaveCount(candidate_ir.get()) == 2,
                  "admit pure block but retain intermediate complete snapshot and final carry inheritance");
            for (u32 sample = 0; sample < 32; ++sample) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                u64 left = seed, right = std::rotl(seed, 19);
                if (sample == 0) { left = 0; right = 0; }
                if (sample == 1) { left = low_mask; right = 1; }
                if (sample == 2) { left = low_mask >> 1; right = 1; }
                if (sample == 3) { left = u64{1} << (bits - 1); right = left; }
                StateStorage a, b, expected;
                for (auto* storage : {&a, &b, &expected}) {
                    storage->Put(0, left); storage->Put(8, right);
                    storage->state->host_cpu_flags = seed;
                }
                be::interp::Interpreter interpreter{*expected.state, reference_ir.get()};
                const auto halt = interpreter.Run();
                Check(reference(a.state) == halt && candidate(b.state) == halt,
                      "optimized and original flags halt match independent interpreter");
                Check(std::memcmp(a.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 96) == 0 &&
                      std::memcmp(b.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 96) == 0,
                      "all masks, all conditions, whole-word GetFlags and DEC outputs match reference");
                Check(a.state->host_cpu_flags == expected.state->host_cpu_flags &&
                      b.state->host_cpu_flags == expected.state->host_cpu_flags,
                      "full final flags including non-arithmetic bits match reference");
                Check((b.Get(40) & (u64{1} << 29)) == (b.Get(32) & (u64{1} << 29)),
                      "DEC preserves CF from preceding ADD across both carry outcomes");
                const auto lhs = left & low_mask, rhs = right & low_mask;
                const auto sum = (lhs + rhs) & low_mask;
                const auto result = (sum - 1) & low_mask;
                const bool carry = bits == 64 ? (lhs + rhs) < lhs : ((lhs + rhs) >> bits) != 0;
                const bool overflow = (((sum ^ 1) & (sum ^ result)) >> (bits - 1)) & 1;
                constexpr u64 arithmetic_bits = 255 | (u64{15} << 28) | (u64{1} << 26);
                const auto expected_flags = (seed & ~arithmetic_bits) | u8(result) |
                        (u64(carry) << 29) | (u64(overflow) << 28) |
                        (u64(result == 0) << 30) | (((result >> (bits - 1)) & 1) << 31) |
                        (u64(((sum ^ 1 ^ result) & 16) != 0) << 26);
                Check(b.state->host_cpu_flags == expected_flags && b.Get(88) == result,
                      "independent C++ full flags oracle including overflow, AF, raw parity and preserved bits");
            }
        }
    }
    std::cout << "PASS 256 admitted pure blocks, 8192 full-state independent references\n";
}

void RejectedBoundaries(Environment& environment) {
    unsigned index{};
    for (const auto* boundary : {"memory", "helper", "poll", "label", "div", "adc", "sbb",
                                 "fault", "barrier", "vector", "uniform-range",
                                 "undefined-logical-flags", "undefined-mul-flags",
                                 "uniform-load-width", "uniform-store-width"}) {
        std::cout << "admission boundary " << boundary << '\n';
        auto block = Program(0x12000 + 16 * index++, T::U64, ir::Flags::All, ir::Cond::CS, boundary);
        const auto before = SaveCount(block.get());
        const bool invalid_span = std::string{boundary} == "uniform-range" ||
                std::string{boundary} == "uniform-load-width" || std::string{boundary} == "uniform-store-width";
        if (invalid_span) {
            bool specific{};
            try { (void)rv::CompileBlock(environment.module, block.get()); }
            catch (const std::runtime_error& error) {
                specific = std::string{error.what()} == "RV64 scalar Uniform access exceeds configured state span";
            }
            Check(specific && block->GetJitCache().jit_state != be::JitState::Cached,
                  std::string{"explicit scalar span rejection: "} + boundary);
        } else {
            Check(rv::CompileBlock(environment.module, block.get()) != nullptr,
                  std::string{"compile rejected-boundary fallback: "} + boundary);
        }
        Check(SaveCount(block.get()) == before,
              std::string{"preserve all writes at observation boundary: "} + boundary);
    }
    std::cout << "PASS rejected memory/helper/poll/label/div/carry/fault/barrier/vector/range boundaries\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        const bool fold = argc == 2 && std::string{argv[1]} == "--fold";
        Environment original{Optimizations::None}, optimized{Optimizations::FlagElimination |
                (fold ? Optimizations::ConstantFolding : Optimizations::None)};
        std::cout << "reference environments prepared\n";
        PositiveReferences(original, optimized);
        if (!fold) RejectedBoundaries(optimized);
        else std::cout << "PASS constant-folding flags references; default mode covers negative admission\n";
        std::cout << "OK " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
