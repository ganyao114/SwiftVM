#include "runtime/backend/arm64/compile.h"
#include "runtime/compile/optimize.h"
#include "runtime/frontend/x86/build_ir.h"
#include "tests/rebuild/function_fixture.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

namespace {
using namespace swift::runtime;
namespace a64 = backend::arm64;
using u64 = std::uint64_t;
using Clock = std::chrono::steady_clock;
struct Options {
    unsigned blocks{32}, requests{128}, samples{5}, warmup{1};
};
struct Counts {
    ir::WorkCounts input{};
    std::size_t native_words{}, frame{}, spills{};
    std::uint32_t entry{};
    u64 code_mix{14695981039346656037ULL};
    friend bool operator==(const Counts&, const Counts&) = default;
};
void Require(bool value) {
    if (!value) throw std::runtime_error("function benchmark rejected actual work");
}

template <bool audit> Counts Run(const swift::tests::function_region::Fixture& fixture,
                                 const swift::x86::DecodedFunction& decoded,
                                 bool supplied) {
    Counts out;
    const auto source = fixture.Window();
    auto* count = audit ? &out.input : nullptr;
    auto built = supplied ? swift::x86::BuildDecodedIR(decoded.instructions,
                                                       decoded.blocks,
                                                       {&source, 1},
                                                       ir::Scope::Function,
                                                       {0, 128, 8},
                                                       a64::KnownTargetInfo(),
                                                       count,
                                                       decoded.entry_block)
                          : swift::x86::BuildFunctionIR({&source, 1},
                                                        fixture.entry,
                                                        {0, 128, 8},
                                                        fixture.Limits(),
                                                        a64::KnownTargetInfo(),
                                                        count);
    Require(bool(built));
    if constexpr (audit) out.entry = built.program.EntryBlock().value;
    auto optimized = compile::Optimize(std::move(built.program));
    Require(bool(optimized));
    auto code = a64::Compile(optimized.program, {136});
    Require(bool(code) && !code.words.empty());
    out.native_words = code.words.size();
    out.frame = code.frame_bytes;
    out.spills = code.spill_slots;
    if constexpr (audit)
        for (const auto word : code.words) out.code_mix = (out.code_mix ^ word) * 1099511628211ULL;
    return out;  // All decoded (auto), IR and native word owners are released here.
}

Options Arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help") {
            std::puts(
                    "function_compile_bench [--blocks 32] [--requests 128] [--samples 5] [--warmup "
                    "1]");
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("missing argument");
        const std::string_view text{argv[i]};
        unsigned value{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
            throw std::runtime_error("invalid number");
        if (key == "--blocks")
            options.blocks = value;
        else if (key == "--requests")
            options.requests = value;
        else if (key == "--samples")
            options.samples = value;
        else if (key == "--warmup")
            options.warmup = value;
        else
            throw std::runtime_error("unknown argument");
    }
    if (!options.blocks || options.blocks > 256 || !options.requests || options.requests > 10000 ||
        !options.samples || options.samples > 100 || options.warmup > 100)
        throw std::runtime_error("arguments exceed fixed limits");
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = Arguments(argc, argv);
        const swift::tests::function_region::Fixture fixture{options.blocks};
        const auto window = fixture.Window();
        const auto decoded =
                swift::x86::DecodeFunction({&window, 1}, fixture.entry, fixture.Limits());
        Require(bool(decoded) && decoded.decode_calls == decoded.instructions.size() &&
                decoded.instructions.size() == options.blocks * 2 + 5 && decoded.budget_exits == 0);
        const auto expected = Run<true>(fixture, decoded, false);
        Require(expected == Run<true>(fixture, decoded, true) &&
                expected.entry == decoded.entry_block && expected.entry != 0);
        // Every request gets the same untimed qualification. Unit IDs are not
        // code identity; actual entry, work counts and machine words are.
        for (unsigned request = 0; request < options.requests; ++request)
            for (const bool supplied : {false, true})
                Require(Run<true>(fixture, decoded, supplied) == expected);
        std::array<std::vector<double>, 2> samples;
        for (auto& row : samples) row.reserve(options.samples);
        u64 consumed{};
        for (unsigned round = 0; round < options.samples + options.warmup; ++round) {
            for (unsigned slot = 0; slot < 2; ++slot) {
                const unsigned mode = (slot + round) % 2;
                const auto start = Clock::now();
                for (unsigned request = 0; request < options.requests; ++request) {
                    const auto result = Run<false>(fixture, decoded, mode == 1);
                    consumed += result.native_words + result.frame + result.spills;
                }
                const auto end = Clock::now();
                if (round >= options.warmup)
                    samples[mode].push_back(
                            std::chrono::duration<double, std::nano>(end - start).count());
            }
        }
        for (unsigned mode = 0; mode < 2; ++mode) {
            std::printf(
                    "{\"measurement_kind\":\"function_region_compile_sequence\",\"synthetic\":true,"
                    "\"partial_isa\":true,\"grants_stage_a_full_or_fex\":false,\"scope\":"
                    "\"function\","
                    "\"input\":\"%s\",\"byte_decode_in_timing\":%s,\"discovery_in_timing\":%s,"
                    "\"input_buffer_materialization_in_timing\":%s,\"publication_in_timing\":false,"
                    "\"build_optimize_allocate_encode_release_in_timing\":true,\"count_work_in_"
                    "timing\":false,"
                    "\"decoder_calls_from_qualified_recipe_per_request\":%zu,\"provided_decoded_"
                    "records\":%zu,"
                    "\"guest_blocks\":%zu,\"entry_block\":%u,\"N_per_request\":%zu,\"E_per_"
                    "request\":%zu,"
                    "\"operand_words_per_request\":%zu,\"ir_blocks_per_request\":%zu,"
                    "\"native_words_per_request\":%zu,\"spill_slots_per_request\":%zu,\"frame_"
                    "bytes\":%zu,"
                    "\"requests_per_sample\":%u,\"samples\":%u,\"warmup\":%u,"
                    "\"qualified_each_request\":true,\"code_mix64\":\"%llu\",\"consumed\":%llu,"
                    "\"sequence_ns_samples\":[",
                    mode ? "provided_decoded" : "automatic_region",
                    mode ? "false" : "true",
                    mode ? "false" : "true",
                    mode ? "false" : "true",
                    mode ? std::size_t{} : decoded.decode_calls,
                    decoded.instructions.size(),
                    decoded.blocks.size(),
                    expected.entry,
                    expected.input.nodes,
                    expected.input.value_uses,
                    expected.input.operand_words,
                    expected.input.blocks,
                    expected.native_words,
                    expected.spills,
                    expected.frame,
                    options.requests,
                    options.samples,
                    options.warmup,
                    static_cast<unsigned long long>(expected.code_mix),
                    static_cast<unsigned long long>(consumed));
            for (std::size_t i = 0; i < samples[mode].size(); ++i)
                std::printf("%s%.0f", i ? "," : "", samples[mode][i]);
            std::puts("]}");
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
