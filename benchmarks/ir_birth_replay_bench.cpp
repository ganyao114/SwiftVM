#include "runtime/ir/builder.h"
#include "runtime/compile/optimize.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace swift::runtime::ir;
using Clock = std::chrono::steady_clock;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
constexpr std::size_t max_requests = 100000, max_events = 2000000, max_atoms = 8000000;
constexpr u64 hash_seed = 14695981039346656037ULL;
u64 Mix(u64 hash, u64 value) { return (hash ^ value) * 1099511628211ULL; }

[[noreturn]] void Fail(std::size_t line, std::string_view message) {
    throw std::runtime_error("line " + std::to_string(line) + ": " + std::string(message));
}
void Require(bool okay, std::size_t line, std::string_view message) {
    if (!okay) Fail(line, message);
}

struct Tokens {
    std::string_view rest;
    std::size_t line;
    std::string_view Take() {
        const auto first = rest.find_first_not_of(" \t");
        if (first == std::string_view::npos) Fail(line, "missing field");
        rest.remove_prefix(first);
        const auto end = rest.find_first_of(" \t");
        const auto token = rest.substr(0, end);
        rest.remove_prefix(token.size());
        return token;
    }
    void End() const {
        Require(rest.find_first_not_of(" \t") == std::string_view::npos, line, "extra field");
    }
    template <class T> T Number() {
        auto token = Take();
        int base = 10;
        if (token.starts_with("0x") || token.starts_with("0X")) {
            token.remove_prefix(2);
            base = 16;
        }
        u64 value{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value, base);
        Require(!token.empty() && result.ec == std::errc{} &&
                        result.ptr == token.data() + token.size(),
                line,
                "invalid unsigned integer");
        Require(value <= std::numeric_limits<T>::max(), line, "integer overflow");
        return static_cast<T>(value);
    }
};

class Lines {
public:
    explicit Lines(const std::string& path) : input(path) {
        if (!input) throw std::runtime_error("cannot open input");
    }
    bool Next() {
        if (!input.getline(buffer.data(), static_cast<std::streamsize>(buffer.size()))) {
            if (input.eof() && !input.gcount()) return false;
            if (!input.eof()) Fail(number + 1, "record exceeds 4095 bytes or input read failed");
        }
        ++number;
        auto length = static_cast<std::size_t>(input.gcount());
        if (!input.eof() && length) --length;  // getline counts its consumed delimiter.
        text = {buffer.data(), length};
        Require(text.find('\0') == std::string_view::npos, number, "NUL in input record");
        if (text.ends_with('\r')) text.remove_suffix(1);
        Require(!text.empty(), number, "empty record");
        for (const unsigned char byte : text) checksum = Mix(checksum, byte);
        checksum = Mix(checksum, '\n');
        return true;
    }
    Tokens Fields() const { return {text, number}; }
    std::string_view Text() const { return text; }
    std::size_t Number() const { return number; }
    u64 Checksum() const { return checksum; }

private:
    std::ifstream input;
    std::array<char, 4096> buffer{};
    std::string_view text;
    std::size_t number{};
    u64 checksum{hash_seed};  // Diagnostic normalized-text hash, not SHA identity.
};

constexpr std::pair<std::string_view, OpCode> op_names[]{
#define INST(name, ...) {#name, OpCode::name},
#include "runtime/ir/opcodes.inc"
#undef INST
};
#define TYPE_NAME(name)                                                                            \
    std::pair { std::string_view{#name}, Type::name }
constexpr auto type_names = std::to_array({
        TYPE_NAME(Void), TYPE_NAME(Bool), TYPE_NAME(U8),   TYPE_NAME(U16),  TYPE_NAME(U32),
        TYPE_NAME(U64),  TYPE_NAME(S8),   TYPE_NAME(S16),  TYPE_NAME(S32),  TYPE_NAME(S64),
        TYPE_NAME(U128), TYPE_NAME(F32),  TYPE_NAME(F64),  TYPE_NAME(V8),   TYPE_NAME(V16),
        TYPE_NAME(V32),  TYPE_NAME(V64),  TYPE_NAME(V128), TYPE_NAME(V256),
});
#undef TYPE_NAME
static_assert(type_names.size() == static_cast<unsigned>(Type::V256) + 1);
template <class Range> auto Named(Tokens& fields, const Range& names) {
    const auto text = fields.Take();
    for (const auto& [name, value] : names)
        if (text == name) return value;
    Fail(fields.line, "unknown declaration name");
}

enum class Kind { Create, Begin, Instruction, Terminal };
struct Event {
    Kind kind{};
    std::size_t line{};
    u32 id{};
    OpCode opcode{};
    Type type{Type::Void};
    OperationMetadata metadata{};
    std::vector<Operand> atoms;
    Terminal terminal;
};
struct Request {
    u64 id{};
    Scope scope{};
    TargetInfo target;
    std::size_t line{};
    std::vector<Event> events;
    bool publish{};
    WorkCounts expected{}, observed{};
    u64 canonical{};
    std::size_t optimized_live_nodes{}, optimized_words{};
    u64 optimized_canonical{};
};

std::vector<Request> Parse(const std::string& path, u64& text_checksum, bool& body_only) {
    Lines lines(path);
    Require(lines.Next(), 1, "missing SVMIR_REPLAY_1 header");
    auto header = lines.Fields();
    Require(header.Take() == "SVMIR_REPLAY_1", 1, "missing SVMIR_REPLAY_1 header");
    const auto semantics = header.Take();
    Require(semantics == "BODY_ONLY" || semantics == "FULL_TERMINALS",
            1,
            "invalid semantics scope");
    body_only = semantics == "BODY_ONLY";
    header.End();
    std::vector<Request> requests;
    Request* current{};
    std::size_t event_total{}, atom_total{};
    while (lines.Next()) {
        auto fields = lines.Fields();
        const auto command = fields.Take();
        if (command == "Q") {
            Require(!current && requests.size() < max_requests,
                    fields.line,
                    "nested Q or request limit");
            requests.emplace_back();
            current = &requests.back();
            current->line = fields.line;
            current->id = fields.Number<u64>();
            const auto scope = fields.Number<unsigned>();
            const auto mode = fields.Number<unsigned>();
            Require(scope <= 1 && mode <= 2, fields.line, "invalid scope or target mode");
            current->scope = static_cast<Scope>(scope);
            current->target.mode = static_cast<TargetMode>(mode);
            current->target.target = fields.Number<u32>();
            current->target.features = fields.Number<u64>();
        } else if (command == "P" || command == "X") {
            Require(current != nullptr, fields.line, "end without Q");
            current->publish = command == "P";
            current = nullptr;
        } else {
            Require(current && ++event_total <= max_events,
                    fields.line,
                    "event outside Q or event limit");
            Event event;
            event.line = fields.line;
            if (command == "C" || command == "B") {
                event.kind = command == "C" ? Kind::Create : Kind::Begin;
                event.id = fields.Number<u32>();
                if (event.kind == Kind::Create) {
                    Require(event.id == current->expected.blocks,
                            fields.line,
                            "C IDs must be dense birth order");
                    ++current->expected.blocks;
                } else
                    Require(event.id < current->expected.blocks,
                            fields.line,
                            "B names undeclared block");
            } else if (command == "I") {
                event.kind = Kind::Instruction;
                event.id = fields.Number<u32>();
                Require(event.id == current->expected.nodes,
                        fields.line,
                        "I IDs must be dense birth order");
                event.opcode = Named(fields, op_names);
                event.type = Named(fields, type_names);
                event.metadata.flag_outputs = fields.Number<u64>();
                const auto profile = fields.Number<unsigned>(), branch = fields.Number<unsigned>();
                Require(profile <= 3 && branch <= 1, fields.line, "invalid operation metadata");
                event.metadata.profile = static_cast<FlagProfile>(profile);
                event.metadata.branch_only = branch;
                const auto count = fields.Number<u32>();
                Require(count <= 1000000 && count <= max_atoms - atom_total,
                        fields.line,
                        "atom limit");
                fields.End();
                fields.rest = {};  // Subsequent V records reuse the line buffer.
                atom_total += count;
                event.atoms.reserve(count);
                for (u32 i = 0; i < count; ++i) {
                    Require(lines.Next(), fields.line, "truncated V sequence");
                    auto atom = lines.Fields();
                    Require(atom.Take() == "V", atom.line, "expected V");
                    Operand value;
                    const auto kind = atom.Number<unsigned>();
                    Require(kind >= 1 && kind <= 9, atom.line, "unknown operand kind");
                    value.kind = static_cast<OperandKind>(kind);
                    value.type = Named(atom, type_names);
                    value.payload = atom.Number<u64>();
                    value.count = atom.Number<u32>();
                    value.code = atom.Number<std::uint16_t>();
                    atom.End();
                    const bool counted = value.kind == OperandKind::Expression ||
                                         value.kind == OperandKind::Arguments;
                    Require(counted ? value.payload == 0 : value.count == 0,
                            atom.line,
                            "nonzero reserved operand field");
                    Require(value.kind == OperandKind::Expression || value.code == 0,
                            atom.line,
                            "reserved operand code");
                    if (value.kind == OperandKind::Value) {
                        Require(value.payload < event.id, atom.line, "value is not already born");
                        ++current->expected.value_uses;
                    }
                    const bool identity =
                            value.kind == OperandKind::Expression &&
                            value.code == static_cast<unsigned>(OperandOp::Identity) &&
                            value.count == 1;
                    if (!identity)
                        current->expected.operand_words +=
                                counted || value.payload <= UINT32_MAX ? 1 : 2;
                    event.atoms.push_back(value);
                }
                ++current->expected.nodes;
            } else if (command == "T") {
                event.kind = Kind::Terminal;
                const auto kind = fields.Number<unsigned>(), has = fields.Number<unsigned>();
                Require(kind >= 1 && kind <= 5 && has <= 1, fields.line, "invalid normal terminal");
                event.terminal.kind = static_cast<TerminalKind>(kind);
                event.terminal.value_present = has;
                event.terminal.value.node = NodeId{fields.Number<u32>()};
                event.terminal.value_type = Named(fields, type_names);
                event.terminal.first.id = BlockId{fields.Number<u32>()};
                event.terminal.second.id = BlockId{fields.Number<u32>()};
                current->expected.value_uses += has;
            } else
                Fail(fields.line, "unknown record");
            current->events.push_back(std::move(event));
        }
        fields.End();
    }
    Require(!current && !requests.empty(), lines.Number(), "unterminated Q or empty input");
    text_checksum = lines.Checksum();
    return requests;
}

AppendResult Write(Builder& builder, Event& event, bool static_writer) {
    if (!static_writer)
        return builder.Append(event.opcode, event.type, event.atoms, event.metadata);
    switch (event.opcode) {
#define INST(name, ...)                                                                            \
    case OpCode::name:                                                                             \
        return builder.Emit<OpCode::name>(event.type, event.atoms, event.metadata);
#include "runtime/ir/opcodes.inc"
#undef INST
        default:
            Fail(event.line, "unknown opcode");
    }
}

u64 Canonical(const Program& program, const Request& request) {
    Require(program.EntryBlock().value == 0, request.line, "replay v1 requires entry 0");
    Require(program.ScopeKind() == request.scope && program.Target() == request.target,
            request.line,
            "published request metadata changed");
    u64 hash = Mix(hash_seed, request.id);
    hash = Mix(hash, static_cast<unsigned>(request.scope));
    hash = Mix(hash, static_cast<unsigned>(request.target.mode));
    hash = Mix(hash, request.target.target);
    hash = Mix(hash, request.target.features);
    u32 active = BlockId::invalid;
    std::size_t next{};
    for (const auto& event : request.events) {
        if (event.kind == Kind::Begin) {
            active = event.id;
            Require(active < program.Blocks().size() && program.Blocks()[active].first_node == next,
                    event.line,
                    "block start changed");
        } else if (event.kind == Kind::Instruction) {
            Require(next < program.Operations().size(), event.line, "missing published operation");
            const auto& op = program.Operations()[next++];
            Require(op.opcode == event.opcode && op.type == event.type &&
                            op.block.value == active && op.Metadata() == event.metadata,
                    event.line,
                    "natural type/opcode/block/metadata mismatch");
            hash = Mix(hash, 0x4f50);
            hash = Mix(hash, event.id);
            hash = Mix(hash, static_cast<unsigned>(op.opcode));
            hash = Mix(hash, static_cast<unsigned>(op.type));
            hash = Mix(hash, op.block.value);
            hash = Mix(hash, op.flag_outputs);
            hash = Mix(hash, static_cast<unsigned>(op.Metadata().profile));
            hash = Mix(hash, op.Metadata().branch_only);
            const auto cells = program.Operands(NodeId{event.id});
            auto at = cells.begin();
            for (const auto& input : event.atoms) {
                if (input.kind == OperandKind::Expression &&
                    input.code == static_cast<unsigned>(OperandOp::Identity) && input.count == 1)
                    continue;
                Require(at != cells.end(), event.line, "missing published operand");
                const auto cell = *at++;
                const bool counted = input.kind == OperandKind::Expression ||
                                     input.kind == OperandKind::Arguments;
                Require(cell.Kind() == input.kind && cell.TypeView() == input.type &&
                                cell.Payload() == (counted ? 0 : input.payload) &&
                                cell.Count() == (counted ? input.count : 0) &&
                                cell.Code() ==
                                        (input.kind == OperandKind::Expression ? input.code : 0),
                        event.line,
                        "operand payload/view/boundary mismatch");
                hash = Mix(hash, static_cast<unsigned>(cell.Kind()));
                hash = Mix(hash, static_cast<unsigned>(cell.TypeView()));
                hash = Mix(hash, cell.Payload());
                hash = Mix(hash, cell.Count());
                hash = Mix(hash, cell.Code());
            }
            Require(at == cells.end(), event.line, "extra published operand");
        } else if (event.kind == Kind::Terminal) {
            Require(active < program.Blocks().size(), event.line, "terminal has no active block");
            const auto& block = program.Blocks()[active];
            const auto& terminal = block.terminal;
            Require(block.State() == BlockState::Closed &&
                            block.first_node + block.node_count == next &&
                            terminal.kind == event.terminal.kind &&
                            terminal.HasValue() == event.terminal.value_present &&
                            terminal.value == event.terminal.value.node &&
                            terminal.value_type == event.terminal.value_type &&
                            terminal.first == event.terminal.first.id &&
                            terminal.second == event.terminal.second.id,
                    event.line,
                    "terminal/range mismatch");
            hash = Mix(hash, 0x5445524d);
            hash = Mix(hash, active);
            hash = Mix(hash, block.first_node);
            hash = Mix(hash, block.node_count);
            hash = Mix(hash, static_cast<unsigned>(terminal.kind));
            hash = Mix(hash, terminal.HasValue());
            hash = Mix(hash, terminal.value.value);
            hash = Mix(hash, static_cast<unsigned>(terminal.value_type));
            hash = Mix(hash, terminal.first.value);
            hash = Mix(hash, terminal.second.value);
            active = BlockId::invalid;
        }
    }
    Require(active == BlockId::invalid && next == program.Operations().size() &&
                    program.Blocks().size() == request.expected.blocks,
            request.line,
            "incomplete published canonical sequence");
    return hash;
}

u64 AuditOptimized(const Program& program) {
    u64 hash = hash_seed;
    const auto operations = program.Operations();
    for (u32 id = 0; id < operations.size(); ++id) {
        const auto& op = operations[id];
        hash = Mix(hash, static_cast<unsigned>(op.opcode));
        hash = Mix(hash, static_cast<unsigned>(op.type));
        hash = Mix(hash, op.block.value);
        hash = Mix(hash, op.flag_outputs);
        hash = Mix(hash, op.properties);
        for (const auto cell : program.Operands(NodeId{id})) {
            if (cell.Kind() == OperandKind::Value)
                Require(cell.Payload() < id && operations[cell.Payload()].type != Type::Void,
                        0,
                        "optimizer left a deleted/forward data use");
            hash = Mix(hash, static_cast<unsigned>(cell.Kind()));
            hash = Mix(hash, static_cast<unsigned>(cell.TypeView()));
            hash = Mix(hash, cell.Payload());
            hash = Mix(hash, cell.Count());
            hash = Mix(hash, cell.Code());
        }
    }
    for (const auto& block : program.Blocks())
        if (block.terminal.HasValue())
            Require(block.terminal.value.value < operations.size() &&
                            operations[block.terminal.value.value].type != Type::Void,
                    0,
                    "optimizer deleted a terminal input");
    return hash;
}

template <bool qualify> void Replay(Request& request, bool static_writer, bool optimize) {
    Builder builder{request.scope, request.target};
    u64 unit{};
    for (auto& event : request.events) {
        if (event.kind == Kind::Create) {
            const auto created = builder.CreateBlock();
            Require(bool(created), event.line, "CreateBlock rejected recipe");
            unit = created.block.unit;
            if constexpr (qualify)
                Require(created.block.id.value == event.id, event.line, "block birth ID mismatch");
        } else if (event.kind == Kind::Begin) {
            Require(builder.BeginBlock({unit, BlockId{event.id}}) == Error::None,
                    event.line,
                    "BeginBlock rejected recipe");
        } else if (event.kind == Kind::Instruction) {
            for (auto& atom : event.atoms)
                if (atom.kind == OperandKind::Value) atom.unit = unit;
            const auto written = Write(builder, event, static_writer);
            Require(bool(written),
                    event.line,
                    "writer rejected recipe (opcode/type/view/operand contract)");
            if constexpr (qualify) {
                Require(written.node.value == event.id &&
                                bool(written.GetValue()) == (event.type != Type::Void),
                        event.line,
                        "node birth ID/natural result mismatch");
                if (written.GetValue())
                    Require(written.GetValue().node == written.node &&
                                    written.GetValue().unit == unit,
                            event.line,
                            "result handle differs from actual birth");
            }
        } else {
            auto terminal = event.terminal;
            if (terminal.value.node) terminal.value.unit = unit;
            if (terminal.first.id) terminal.first.unit = unit;
            if (terminal.second.id) terminal.second.unit = unit;
            Require(builder.EndBlock(terminal) == Error::None,
                    event.line,
                    "EndBlock rejected recipe");
        }
    }
    if constexpr (qualify) {
        request.observed = builder.CountWork();
        Require(request.observed == request.expected,
                request.line,
                "actual N/E/words/blocks differ from recipe");
    }
    if (request.publish) {
        auto submitted = builder.Submit();
        Require(bool(submitted), request.line, "P requires a complete normal-terminal request");
        if constexpr (qualify) request.canonical = Canonical(submitted.program, request);
        if (optimize) {
            auto result = swift::runtime::compile::Optimize(std::move(submitted.program));
            Require(bool(result), request.line, "optimizer failed (request not dropped)");
            if constexpr (qualify) {
                Require(result.program.UnitId() == unit &&
                                result.program.Operations().size() == request.observed.nodes,
                        request.line,
                        "optimizer changed owner/node identity");
                request.optimized_words = result.program.OperandArena().size();
                for (const auto& op : result.program.Operations())
                    request.optimized_live_nodes += op.opcode != OpCode::Nop;
                request.optimized_canonical = AuditOptimized(result.program);
            }
        }
    }  // Program/Builder destruction is inside the caller's request timer; X never submits.
}

struct Options {
    std::string path;
    std::size_t samples{5}, warmup{1};
    bool static_writer{true}, optimize{};
};
Options Arguments(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view key{argv[i]};
        if (key == "--help") {
            std::puts(
                    "ir_birth_replay_bench --input trace.svmir [--writer static|generic] "
                    "[--pipeline writer|optimize] [--samples 5] [--warmup 1]");
            std::exit(0);
        }
        if (++i == argc) throw std::runtime_error("missing option value");
        const std::string_view value{argv[i]};
        if (key == "--input")
            options.path = value;
        else if (key == "--writer") {
            if (value != "static" && value != "generic") throw std::runtime_error("unknown writer");
            options.static_writer = value == "static";
        } else if (key == "--pipeline") {
            if (value != "writer" && value != "optimize")
                throw std::runtime_error("unknown pipeline");
            options.optimize = value == "optimize";
        } else if (key == "--samples" || key == "--warmup") {
            Tokens field{value, 0};
            const auto number = field.Number<std::size_t>();
            field.End();
            if (number > 1000 || (key == "--samples" && !number))
                throw std::runtime_error("sample limit");
            (key == "--samples" ? options.samples : options.warmup) = number;
        } else
            throw std::runtime_error("unknown option");
    }
    if (options.path.empty()) throw std::runtime_error("--input is required");
    return options;
}

void PrintSamples(const std::vector<double>& values, double divisor = 1) {
    std::putchar('[');
    for (std::size_t i = 0; i < values.size(); ++i)
        std::printf("%s%.3f", i ? "," : "", values[i] / divisor);
    std::putchar(']');
}
void Report(const std::vector<Request>& requests,
            const Options& options,
            Scope scope,
            const std::vector<double>& samples,
            u64 text_checksum,
            bool body_only) {
    std::size_t count{}, published{}, optimized_live{}, optimized_words{};
    WorkCounts total;
    for (const auto& request : requests)
        if (request.scope == scope) {
            ++count;
            published += request.publish;
            total.nodes += request.observed.nodes;
            total.value_uses += request.observed.value_uses;
            total.operand_words += request.observed.operand_words;
            total.blocks += request.observed.blocks;
            optimized_live += request.optimized_live_nodes;
            optimized_words += request.optimized_words;
        }
    if (!count) return;
    std::printf(
            "{\"measurement_kind\":\"%s\",\"qualification\":\"%s\","
            "\"grants_stage_a_full_or_fex\":false,\"writer\":\"%s\",\"scope\":\"%s\","
            "\"semantics_scope\":\"%s\",\"body_only\":%s,"
            "\"samples\":%zu,\"warmup\":%zu,\"attempts_per_sample\":%zu,\"published_per_sample\":%"
            "zu,"
            "\"abandoned_per_sample\":%zu,\"measured_attempts_total\":%llu,"
            "\"N_per_sample\":%zu,\"E_per_sample\":%zu,\"words_per_sample\":%zu,\"blocks_per_"
            "sample\":%zu,"
            "\"opcode_dispatch_in_timing\":true,\"unit_materialization_in_timing\":true,"
            "\"parse_and_payload_audit_in_timing\":false,\"count_work_in_timing\":false,"
            "\"request_order\":\"input\",\"per_request_timer_overhead_included\":true,"
            "\"abandoned_payload_canonical\":false,\"normalized_text_fnv1a64\":\"%llu\",\"sequence_"
            "ns_samples\":",
            options.optimize ? "birth_ir_writer_optimize_replay" : "birth_ir_writer_replay",
            options.optimize ? "writer_contract_and_optimizer_structural_output"
                             : "writer_contract_only",
            options.static_writer ? "static" : "generic",
            scope == Scope::Block ? "block" : "function",
            body_only ? "BODY_ONLY" : "FULL_TERMINALS",
            body_only ? "true" : "false",
            options.samples,
            options.warmup,
            count,
            published,
            count - published,
            static_cast<unsigned long long>(count) * options.samples,
            total.nodes,
            total.value_uses,
            total.operand_words,
            total.blocks,
            static_cast<unsigned long long>(text_checksum));
    PrintSamples(samples);
    std::printf(",\"ns_per_attempt_samples\":");
    PrintSamples(samples, static_cast<double>(count));
    std::printf(
            ",\"optimizer_in_timing\":%s,\"optimizer_requests_per_sample\":%zu,"
            "\"optimized_live_nodes_per_sample\":%zu,\"optimized_physical_words_per_sample\":%zu",
            options.optimize ? "true" : "false",
            options.optimize ? published : 0,
            optimized_live,
            optimized_words);
    std::printf(",\"requests\":[");
    bool first = true;
    for (const auto& request : requests)
        if (request.scope == scope) {
            std::printf(
                    "%s{\"id\":%llu,\"outcome\":\"%s\",\"N\":%zu,\"E\":%zu,\"words\":%zu,"
                    "\"blocks\":%zu,"
                    "\"published_payload_canonical\":%s,\"canonical_word_mix64\":\"%llu\","
                    "\"optimized_live_nodes\":%zu,\"optimized_words\":%zu,\"optimized_word_mix64\":"
                    "\"%llu\"}",
                    first ? "" : ",",
                    static_cast<unsigned long long>(request.id),
                    request.publish ? "published" : "abandoned",
                    request.observed.nodes,
                    request.observed.value_uses,
                    request.observed.operand_words,
                    request.observed.blocks,
                    request.publish ? "true" : "false",
                    static_cast<unsigned long long>(request.canonical),
                    request.optimized_live_nodes,
                    request.optimized_words,
                    static_cast<unsigned long long>(request.optimized_canonical));
            first = false;
        }
    std::puts("]}");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        const auto options = Arguments(argc, argv);
        u64 text_checksum{};
        bool body_only{};
        auto requests = Parse(options.path, text_checksum, body_only);
        for (auto& request : requests)
            Replay<true>(request, options.static_writer, options.optimize);
        std::array<std::vector<double>, 2> samples;
        for (auto& scope : samples) scope.reserve(options.samples);
        for (std::size_t sample = 0; sample < options.samples + options.warmup; ++sample) {
            std::array<double, 2> elapsed{};
            for (auto& request : requests) {
                const auto begin = Clock::now();
                Replay<false>(request, options.static_writer, options.optimize);
                const auto end = Clock::now();
                elapsed[static_cast<unsigned>(request.scope)] +=
                        std::chrono::duration<double, std::nano>(end - begin).count();
            }
            if (sample >= options.warmup)
                for (unsigned scope = 0; scope < 2; ++scope)
                    samples[scope].push_back(elapsed[scope]);
        }
        Report(requests, options, Scope::Block, samples[0], text_checksum, body_only);
        Report(requests, options, Scope::Function, samples[1], text_checksum, body_only);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "birth replay rejected: %s\n", error.what());
        return 1;
    }
}
