#include "runtime/backend/arm64/compile.h"
#include "runtime/ir/optimize.h"
#include "runtime/ir/builder.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace swift::runtime;
using namespace ir;
namespace a64 = backend::arm64;
using Clock = std::chrono::steady_clock;
using u64 = std::uint64_t;

struct Counts {
    WorkCounts input{};
    std::size_t words{}, spills{}, frame{};
};

struct Options {
    unsigned requests{128}, nodes{256}, samples{5}, warmup{1};
    bool flags{};
};

void Require(bool okay) {
    if (!okay) throw std::runtime_error("scalar compile benchmark rejected actual work");
}

Value ValueOf(AppendResult result) {
    Require(bool(result));
    return result.GetValue();
}

void Check(Error error) { Require(error == Error::None); }

// Synthetic input intentionally depends on guest state. Arithmetic cannot be
// folded to a constant; the initial live set exercises real spill decisions.
template <bool audit, bool flags> Counts CompileOne(Scope scope, unsigned nodes) {
    Builder builder{scope, a64::KnownTargetInfo()};
    std::array<BlockRef, 4> blocks{};
    const unsigned count = scope == Scope::Block ? 1 : 4;
    for (unsigned block = 0; block < count; ++block) {
        const auto created = builder.CreateBlock();
        Require(bool(created));
        blocks[block] = created.block;
    }
    Value value;
    for (unsigned block = 0; block < count; ++block) {
        Check(builder.BeginBlock(blocks[block]));
        if (block == 0) {
            std::array<Value, 32> live{};
            for (unsigned i = 0; i < live.size(); ++i)
                live[i] = ValueOf(builder.Emit<OpCode::LoadUniform>(
                        Type::U64, {Operand::Uniform(i * 8, Type::U64)}));
            value = live[0];
            for (unsigned i = 1; i < live.size(); ++i)
                value = ValueOf(builder.Emit<OpCode::Add>(
                        Type::U64, {Operand::ValueOf(value), Operand::ValueOf(live[i])}));
        }
        for (unsigned i = 0; i < nodes / count; ++i) {
            value = ValueOf(builder.Emit<OpCode::Add>(
                    Type::U64,
                    {Operand::ValueOf(value), Operand::Immediate(1 + i % 4093, Type::U64)},
                    flags ? OperationMetadata{FlagBits(Flag::All),
                                              FlagProfile::ArithmeticRaw,
                                              false}
                          : OperationMetadata{}));
            if (flags && i % 16 == 0) {
                const auto condition = ValueOf(builder.Emit<OpCode::CondSet>(
                        Type::Bool, {Operand::Condition(Condition::GE)}));
                value = ValueOf(builder.Emit<OpCode::Select>(Type::U64,
                                                             {Operand::ValueOf(condition),
                                                              Operand::ValueOf(value),
                                                              Operand::ValueOf(value)}));
            }
        }
        if (block + 1 < count)
            Check(builder.EndBlock(Terminal::Jump(blocks[block + 1])));
        else {
            Require(bool(builder.Emit<OpCode::StoreUniform>(
                    Type::Void, {Operand::Uniform(512, Type::U64), Operand::ValueOf(value)})));
            Check(builder.EndBlock(Terminal::Return(value)));
        }
    }
    Counts result;
    if constexpr (audit) result.input = builder.CountWork();
    auto submitted = builder.Submit();
    Require(bool(submitted));
    auto optimized = ir::Optimize(std::move(submitted.program));
    Require(bool(optimized));
    auto compiled = a64::Compile(optimized.program,
                                 flags ? a64::CompileOptions{768} : a64::CompileOptions{});
    Require(bool(compiled));
    result.words = compiled.words.size();
    result.spills = compiled.spill_slots;
    result.frame = compiled.frame_bytes;
    Require(result.words != 0);
    return result;  // Both Program and machine code release before returning.
}

Options Arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help") {
            std::puts(
                    "ir_compile_bench [--requests 128] [--nodes 256] [--samples 5] [--warmup 1] "
                    "[--flags on|off]");
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("missing option value");
        const std::string_view text{argv[i]};
        unsigned value{};
        if (key == "--flags") {
            if (text != "on" && text != "off") throw std::runtime_error("invalid flags mode");
            options.flags = text == "on";
            continue;
        }
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            throw std::runtime_error("invalid number");
        if (key == "--requests")
            options.requests = value;
        else if (key == "--nodes")
            options.nodes = value;
        else if (key == "--samples")
            options.samples = value;
        else if (key == "--warmup")
            options.warmup = value;
        else
            throw std::runtime_error("unknown option");
    }
    if (!options.requests || options.requests > 10000 || options.nodes < 4 ||
        options.nodes > 100000 || !options.samples || options.samples > 100 || options.warmup > 100)
        throw std::runtime_error("option exceeds fixed limits");
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = Arguments(argc, argv);
        std::array<Counts, 2> expected;
        std::array<std::vector<double>, 2> samples;
        for (const auto scope : {Scope::Block, Scope::Function}) {
            expected[unsigned(scope)] = options.flags
                                                ? CompileOne<true, true>(scope, options.nodes)
                                                : CompileOne<true, false>(scope, options.nodes);
            samples[unsigned(scope)].reserve(options.samples);
        }
        u64 consumed{};
        for (unsigned round = 0; round < options.samples + options.warmup; ++round) {
            // Alternate scope order across rounds, without adaptive batching.
            for (unsigned slot = 0; slot < 2; ++slot) {
                const auto scope = static_cast<Scope>((slot + round) % 2);
                const auto start = Clock::now();
                for (unsigned request = 0; request < options.requests; ++request) {
                    const auto result = options.flags
                                                ? CompileOne<false, true>(scope, options.nodes)
                                                : CompileOne<false, false>(scope, options.nodes);
                    consumed += result.words + result.spills + result.frame;
                }
                const auto end = Clock::now();
                if (round >= options.warmup)
                    samples[unsigned(scope)].push_back(
                            std::chrono::duration<double, std::nano>(end - start).count());
            }
        }
        for (unsigned scope = 0; scope < 2; ++scope) {
            const auto& c = expected[scope];
            std::printf(
                    "{\"measurement_kind\":\"scalar_ir_compile_sequence\",\"synthetic\":true,"
                    "\"grants_stage_a_full_or_fex\":false,\"scope\":\"%s\",\"decode_present\":"
                    "false,"
                    "\"build_optimize_allocate_encode_release_in_timing\":true,\"publication_in_"
                    "timing\":false,"
                    "\"count_work_in_timing\":false,\"requests_per_sample\":%u,\"samples\":%u,"
                    "\"warmup\":%u,"
                    "\"flags_enabled\":%s,\"N_per_request\":%zu,\"E_per_request\":%zu,\"words_per_"
                    "request\":%zu,\"blocks_per_request\":%zu,"
                    "\"native_words_per_request\":%zu,\"spill_slots_per_request\":%zu,\"frame_"
                    "bytes\":%zu,"
                    "\"consumed\":%llu,\"sequence_ns_samples\":[",
                    scope ? "function" : "block",
                    options.requests,
                    options.samples,
                    options.warmup,
                    options.flags ? "true" : "false",
                    c.input.nodes,
                    c.input.value_uses,
                    c.input.operand_words,
                    c.input.blocks,
                    c.words,
                    c.spills,
                    c.frame,
                    static_cast<unsigned long long>(consumed));
            for (std::size_t i = 0; i < samples[scope].size(); ++i)
                std::printf("%s%.0f", i ? "," : "", samples[scope][i]);
            std::puts("]}");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
