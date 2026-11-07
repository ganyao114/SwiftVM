#include "base/error.h"
#include "base/logging.h"
#include "tests/rebuild/x86_fixtures.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <stdexcept>
#include <vector>

namespace {
using namespace swift::runtime::ir;
using namespace swift::tests::x86_ir;
using Clock = std::chrono::steady_clock;

// Keep canonical_semantic_v1 identical in both benchmark translation units.
// This read-only diagnostic shim is also compiled against the saved baseline.
struct Semantics {
    std::uint64_t checksum{1469598103934665603ull};
    std::size_t stored_cells{}, logical_cells{}, physical_words{}, values{}, terminals{};
};

std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) {
    return (hash ^ value) * 1099511628211ull;
}

Semantics ObserveProgram(const Program& p) {
#if !defined(SVM_IR_BASELINE_LAYOUT) && !defined(SVM_IR_BASELINE_COMPACT)
    if (p.EntryBlock().value != 0) swift::ThrowError("canonical_semantic_v1 requires entry 0");
#endif
    Semantics out;
    auto& hash = out.checksum;
    hash = Mix(hash, static_cast<unsigned>(p.ScopeKind()));
    const auto target = p.Target();
    hash = Mix(hash, static_cast<unsigned>(target.mode));
    hash = Mix(hash, target.target);
    hash = Mix(hash, target.features);
    hash = Mix(hash, p.Operations().size());
    hash = Mix(hash, p.Blocks().size());
#ifdef SVM_IR_BASELINE_LAYOUT
    out.physical_words = p.FlatOperands().size() * 2;  // Historical Cell16.
#elif defined(SVM_IR_BASELINE_COMPACT)
    out.physical_words = p.FlatOperands().WordCount();
#else
    out.physical_words = p.OperandArena().size();
#endif
#if !defined(SVM_IR_BASELINE_LAYOUT) && !defined(SVM_IR_BASELINE_COMPACT)
    std::uint32_t local_id{};
#endif
    for (const auto& op : p.Operations()) {
#ifdef SVM_IR_BASELINE_LAYOUT
        const auto metadata = op.metadata;
#else
        const auto metadata = op.Metadata();
#endif
        hash = Mix(hash, 0x4f50424547494eull);  // operation boundary, not a physical offset
        hash = Mix(hash, static_cast<unsigned>(op.opcode));
        hash = Mix(hash, static_cast<unsigned>(op.type));
        hash = Mix(hash, op.block.value);
        hash = Mix(hash, metadata.flag_outputs);
        hash = Mix(hash, static_cast<unsigned>(metadata.profile));
        hash = Mix(hash, metadata.branch_only);
#if defined(SVM_IR_BASELINE_LAYOUT) || defined(SVM_IR_BASELINE_COMPACT)
        const auto operands = p.Operands(op);
#else
        const auto operands = p.Operands(NodeId{local_id++});
#endif
        for (const auto cell : operands) {
#ifdef SVM_IR_BASELINE_LAYOUT
            const auto kind = cell.kind;
            const auto view = cell.type;
            const auto payload = cell.payload;
            const auto count = cell.count;
            const auto code = cell.code;
#else
            const auto kind = cell.Kind();
            const auto view = cell.TypeView();
            const auto payload = cell.Payload();
            const auto count = cell.Count();
            const auto code = cell.Code();
#endif
            ++out.stored_cells;
            // Identity(1) and the same direct atom have identical semantics.
            if (kind == OperandKind::Expression &&
                code == static_cast<unsigned>(OperandOp::Identity) && count == 1)
                continue;
            ++out.logical_cells;
            out.values += kind == OperandKind::Value;
            hash = Mix(hash, 0x43454c4cull);
            hash = Mix(hash, static_cast<unsigned>(kind));
            hash = Mix(hash, static_cast<unsigned>(view));
            hash = Mix(hash, payload);
            hash = Mix(hash, count);
            hash = Mix(hash, code);
        }
        hash = Mix(hash, 0x4f50454e44ull);
    }
    for (const auto& block : p.Blocks()) {
#ifdef SVM_IR_BASELINE_LAYOUT
        const auto state = block.state;
        const auto has_value = block.terminal.value_present;
        const auto value = block.terminal.value.node.value;
        const auto first = block.terminal.first.id.value, second = block.terminal.second.id.value;
#else
        const auto state = block.State();
        const auto has_value = block.terminal.HasValue();
        const auto value = block.terminal.value.value;
        const auto first = block.terminal.first.value, second = block.terminal.second.value;
#endif
        out.terminals += has_value;
        hash = Mix(hash, 0x424c4f434bull);
        hash = Mix(hash, block.first_node);
        hash = Mix(hash, block.node_count);
        hash = Mix(hash, static_cast<unsigned>(state));
        hash = Mix(hash, static_cast<unsigned>(block.terminal.kind));
        hash = Mix(hash, has_value);
        hash = Mix(hash, static_cast<unsigned>(block.terminal.value_type));
        hash = Mix(hash, value);
        hash = Mix(hash, first);
        hash = Mix(hash, second);
    }
    return out;
}

double Nanoseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - begin).count();
}

void Numbers(const std::vector<double>& values) {
    std::putchar('[');
    for (std::size_t i = 0; i < values.size(); ++i) std::printf("%s%.3f", i ? "," : "", values[i]);
    std::putchar(']');
}
enum class Mode { Sequential, BatchDiagnostic };

struct Options {
    std::size_t requests{32}, samples{9}, warmup{1};
    Mode mode{Mode::Sequential};
};

Options Parse(int argc, char** argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help") {
            std::puts(
                    "x86_build_submit_bench [--requests 32] [--samples 9] [--warmup 1] [--mode "
                    "sequential|batch-diagnostic]");
            std::exit(0);
        }
        if (++i == argc) swift::ThrowError("missing option value");
        const std::string value = argv[i];
        if (key == "--requests")
            out.requests = std::stoull(value);
        else if (key == "--samples")
            out.samples = std::stoull(value);
        else if (key == "--warmup")
            out.warmup = std::stoull(value);
        else if (key == "--mode" && value == "sequential")
            out.mode = Mode::Sequential;
        else if (key == "--mode" && value == "batch-diagnostic")
            out.mode = Mode::BatchDiagnostic;
        else
            swift::ThrowError("invalid option");
    }
    if (!out.requests || out.requests > 4096 || out.samples < 3 || out.samples > 1000 ||
        out.warmup > 1000)
        swift::ThrowError("invalid request/sample count");
    return out;
}

constexpr std::size_t negative_count = 3;

void UnsupportedRequest(Scope scope,
                        TargetInfo target,
                        std::size_t which,
                        WorkCounts* collected = nullptr) {
    constexpr std::array<std::uint8_t, 2> ud2{0x0f, 0x0b};
    constexpr std::array<std::uint8_t, 12> prefix{
            0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x0f, 0x0b};
    constexpr std::array<std::uint8_t, 3> late_rejection{0x0f, 0x22, 0xc0};
    const std::span<const std::uint8_t> bytes =
            which == 0   ? std::span<const std::uint8_t>{ud2}
            : which == 1 ? std::span<const std::uint8_t>{prefix}
                         : std::span<const std::uint8_t>{late_rejection};
    const std::array<swift::x86::BlockWindow, 1> rejected{{{0x2000, bytes}}};
    const auto result = swift::x86::BuildIR(rejected, scope, layout, target, collected);
    if (result.error != swift::x86::BuildError::Unsupported || result.program ||
        result.instruction_count != (which == 1 ? 1u : 0u) ||
        result.decoded_bytes != (which == 1 ? 10u : 0u))
        swift::ThrowError("fixed negative changed outcome");
    if (collected) {
        constexpr std::array<WorkCounts, negative_count> expected{
                {{0, 0, 0, 1}, {2, 1, 4, 1}, {1, 0, 1, 1}}};
        if (*collected != expected[which]) swift::ThrowError("actual abandoned prefix differs");
    }
}

void RejectedSequence(Scope scope, TargetInfo target) {
    for (std::size_t i = 0; i < negative_count; ++i) UnsupportedRequest(scope, target, i);
}

struct Facts {
    Semantics semantics;
    std::uint64_t sequence_checksum{1469598103934665603ull};
    std::size_t N{}, bytes{}, instructions{}, storage{}, reserved{};
    std::array<WorkCounts, negative_count> abandoned{};
};

Facts Verify(std::span<const swift::x86::BlockWindow> input,
             Scope scope,
             TargetInfo target,
             std::size_t requests) {
    Facts facts;
    for (std::size_t i = 0; i < requests; ++i) {
        const auto result = swift::x86::BuildIR(input, scope, layout, target);
        if (!result || result.program.ScopeKind() != scope || result.program.Target() != target)
            swift::ThrowError("real-byte fixture rejected");
        const auto observed = ObserveProgram(result.program);
        if (!i) {
            facts.semantics = observed;
            facts.N = result.program.Operations().size();
            facts.bytes = result.decoded_bytes;
            facts.instructions = result.instruction_count;
            facts.storage = result.program.StorageBytes();
            facts.reserved = result.program.ReservedBytes();
        } else if (observed.checksum != facts.semantics.checksum)
            swift::ThrowError("untimed fixed sequence semantics differ");
        facts.sequence_checksum = Mix(facts.sequence_checksum, observed.checksum);
    }
    for (std::size_t i = 0; i < negative_count; ++i) {
        UnsupportedRequest(scope, target, i, &facts.abandoned[i]);
        for (const auto count : {facts.abandoned[i].nodes,
                                 facts.abandoned[i].value_uses,
                                 facts.abandoned[i].operand_words,
                                 facts.abandoned[i].blocks})
            facts.sequence_checksum = Mix(facts.sequence_checksum, count);
    }
    return facts;
}

void Measure(const Options& options, Scope scope, TargetMode mode) {
    const auto input = scope == Scope::Block ? std::span{windows}.first(1) : std::span{windows};
    const TargetInfo target{mode, 0xa64, 0x42};
    const auto facts = Verify(input, scope, target, options.requests);
    const bool sequential = options.mode == Mode::Sequential;
    std::vector<swift::x86::BuildResult> slots(sequential ? 0 : options.requests);
    std::vector<double> sequence, construction, destruction, rejected;
    std::size_t measured_attempts{}, published_N{}, published_words{}, decoded_bytes{},
            instructions{};
    for (std::size_t sample = 0; sample < options.samples + options.warmup; ++sample) {
        std::size_t actual_N{}, actual_words{}, actual_bytes{}, actual_instructions{};
        const auto account = [&](const swift::x86::BuildResult& result) {
            if (!result) swift::ThrowError("timed real-byte request rejected");
#ifdef SVM_IR_BASELINE_LAYOUT
            const auto words = result.program.FlatOperands().size() * 2;
#elif defined(SVM_IR_BASELINE_COMPACT)
            const auto words = result.program.FlatOperands().WordCount();
#else
            const auto words = result.program.OperandArena().size();
#endif
            const auto nodes = result.program.Operations().size();
            if (nodes != facts.N || words != facts.semantics.physical_words ||
                result.decoded_bytes != facts.bytes ||
                result.instruction_count != facts.instructions)
                swift::ThrowError("timed request differs from verified recipe");
            actual_N += nodes;
            actual_words += words;
            actual_bytes += result.decoded_bytes;
            actual_instructions += result.instruction_count;
        };
        const auto begin = Clock::now();
        if (sequential) {
            for (std::size_t i = 0; i < options.requests; ++i) {
                const auto result = swift::x86::BuildIR(input, scope, layout, target);
                account(result);
            }  // Returned Program and request destruction are inside primary time.
            RejectedSequence(scope, target);
            const auto end = Clock::now();
            if (sample >= options.warmup) sequence.push_back(Nanoseconds(begin, end));
        } else {
            for (auto& slot : slots) {
                slot = swift::x86::BuildIR(input, scope, layout, target);
                account(slot);
            }
            const auto built = Clock::now();
            for (const auto& slot : slots)
                if (ObserveProgram(slot.program).checksum != facts.semantics.checksum)
                    swift::ThrowError("batch Program semantics differ");
            const auto destroy_begin = Clock::now();
            for (auto& slot : slots) slot.program = Program{};
            const auto destroyed = Clock::now();
            RejectedSequence(scope, target);
            const auto end = Clock::now();
            if (sample >= options.warmup) {
                construction.push_back(Nanoseconds(begin, built));
                destruction.push_back(Nanoseconds(destroy_begin, destroyed));
                rejected.push_back(Nanoseconds(destroyed, end));
            }
        }
        if (sample >= options.warmup) {
            measured_attempts += options.requests + negative_count;
            published_N += actual_N;
            published_words += actual_words;
            decoded_bytes += actual_bytes;
            instructions += actual_instructions;
        }
    }
    const char* name = scope == Scope::Block ? "block" : "function";
    const char* target_mode = mode == TargetMode::Known      ? "known"
                              : mode == TargetMode::EarlyOff ? "early_off"
                                                             : "unknown";
    std::printf(
            "{\"scope\":\"%s\",\"target_mode\":\"%s\",\"blocks_per_positive\":%zu,"
            "\"positive_N\":%zu,\"positive_E_verified\":%zu,\"positive_logical_cells\":%zu,"
            "\"positive_physical_words\":%zu,\"canonical_sequence_checksum\":\"%llu\","
            "\"storage_bytes_per_positive\":%zu,\"reserved_bytes_per_positive\":%zu,"
            "\"attempts_per_sequence\":%zu,\"published_per_sequence\":%zu,\"failed_per_sequence\":"
            "3,"
            "\"abandoned_per_sequence\":3,\"abandoned_prefix_N_E_words\":[%zu,%zu,%zu],"
            "\"negative_requests\":[\"UD2\",\"MOVabs_then_UD2\",\"unsupported_MOV_with_partial_"
            "IR\"],\"abandoned_work_basis\":\"actual_untimed_owner_snapshot_before_release\","
            "\"measured_attempts_total\":%zu,\"warmup_attempts_total\":%zu,\"untimed_verification_"
            "attempts\":%zu,"
            "\"actual_published_N_total\":%zu,\"actual_published_words_total\":%zu,"
            "\"E_total_from_verified_recipe\":%zu,\"actual_decoded_bytes_total\":%zu,\"actual_"
            "instructions_total\":%zu,"
            "\"sequence_ns_samples\":",
            name,
            target_mode,
            input.size(),
            facts.N,
            facts.semantics.values + facts.semantics.terminals,
            facts.semantics.logical_cells,
            facts.semantics.physical_words,
            static_cast<unsigned long long>(facts.sequence_checksum),
            facts.storage,
            facts.reserved,
            options.requests + negative_count,
            options.requests,
            facts.abandoned[0].nodes + facts.abandoned[1].nodes + facts.abandoned[2].nodes,
            facts.abandoned[0].value_uses + facts.abandoned[1].value_uses +
                    facts.abandoned[2].value_uses,
            facts.abandoned[0].operand_words + facts.abandoned[1].operand_words +
                    facts.abandoned[2].operand_words,
            measured_attempts,
            (options.requests + negative_count) * options.warmup,
            options.requests + negative_count,
            published_N,
            published_words,
            (facts.semantics.values + facts.semantics.terminals) * options.requests *
                    options.samples,
            decoded_bytes,
            instructions);
    Numbers(sequence);
    if (sequential) {
        auto sorted = sequence;
        std::sort(sorted.begin(), sorted.end());
        std::printf(
                ",\"sequence_build_submit_release_ns_median\":%.3f,\"ns_per_attempt_including_"
                "rejection_and_release\":%.3f",
                sorted[sorted.size() / 2],
                sorted[sorted.size() / 2] / (options.requests + negative_count));
    } else {
        std::printf(",\"diagnostic_decode_build_submit_ns_samples\":");
        Numbers(construction);
        std::printf(",\"diagnostic_destruction_ns_samples\":");
        Numbers(destruction);
        std::printf(",\"diagnostic_rejected_request_ns_samples\":");
        Numbers(rejected);
    }
    std::putchar('}');
}
}  // namespace

int main(int argc, char** argv) {
    swift::log::SetSink(&swift::log::StandardErrorSink());
    try {
        const auto options = Parse(argc, argv);
        const bool sequential = options.mode == Mode::Sequential;
#ifdef SVM_IR_BASELINE_LAYOUT
        constexpr const char* layout_name = "baseline_cell16";
#else
        constexpr const char* layout_name = "compact_word8";
#endif
        std::printf(
                "{\"schema\":4,\"checksum_schema\":\"canonical_semantic_v1\",\"layout\":\"%s\","
                "\"fixture\":\"x86_mov_lea_control_v1_with_abandoned_prefixes\",\"actual_byte_"
                "decode\":true,"
                "\"application_workload\":false,\"partial_ISA\":true,\"full_compile_or_fex_"
                "comparison\":false,"
                "\"passes_analysis_ssa_cfg_ra_backend_run\":false,\"allocation_tracker\":false,"
                "\"semantic_verification\":\"complete_untimed_replay_of_same_fixed_sequence\","
                "\"timed_programs_individually_checksummed\":%s,\"timed_E_basis\":\"untimed_"
                "verified_recipe\","
                "\"request_accounting_in_timing\":true,\"mode\":\"%s\",\"primary_latency_"
                "available\":%s,"
                "\"requests\":%zu,\"samples\":%zu,\"warmup_sequences\":%zu,\"cases\":[",
                layout_name,
                sequential ? "false" : "true",
                sequential ? "sequential" : "batch-diagnostic",
                sequential ? "true" : "false",
                options.requests,
                options.samples,
                options.warmup);
        bool comma = false;
        for (const auto scope : {Scope::Block, Scope::Function})
            for (const auto mode : {TargetMode::Known, TargetMode::EarlyOff, TargetMode::Unknown}) {
                if (comma) std::putchar(',');
                comma = true;
                Measure(options, scope, mode);
            }
        std::puts("]}");
        return 0;
    } catch (const std::exception& error) {
        SVM_LOG(Error, Runtime, "x86 construction benchmark failed: {}", error.what());
        return 1;
    }
}
