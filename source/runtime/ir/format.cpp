#include "format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>

#include "base/format.h"
#include "program.h"

namespace swift::runtime::ir {
namespace {
std::string_view TypeName(Type type) {
    constexpr std::array<std::string_view, 20> names{
            "void", "bool", "u8",  "u16", "u32", "u64", "s8",  "s16",  "s32",  "s64",
            "u128", "f32",  "f64", "v8",  "v16", "v32", "v64", "v128", "v256", "?"};
    const auto index = static_cast<unsigned>(type);
    return names[std::min(index, unsigned(names.size() - 1))];
}

class Text {
public:
    explicit Text(FormatSink sink) : sink(sink) {}

    class Iterator {
    public:
        using iterator_category = std::output_iterator_tag;
        using value_type = void;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using reference = void;

        explicit Iterator(Text& text) : text(&text) {}

        Iterator& operator*() { return *this; }

        Iterator& operator++() { return *this; }

        Iterator operator++(int) { return *this; }

        Iterator& operator=(char value) {
            text->Append({&value, 1});
            return *this;
        }

    private:
        Text* text;
    };

    void Append(std::string_view value) {
        while (!value.empty()) {
            const auto count = std::min(value.size(), storage.size() - used);
            std::memcpy(storage.data() + used, value.data(), count);
            used += count;
            value.remove_prefix(count);
            if (used == storage.size()) Flush();
        }
    }

    template <typename... Args> void Print(fmt::format_string<Args...> format, Args&&... args) {
        swift::FormatTo(Iterator{*this}, format, std::forward<Args>(args)...);
    }

    void Flush() {
        if (used) sink.write(sink.context, {storage.data(), used});
        used = 0;
    }

private:
    FormatSink sink;
    std::array<char, 512> storage{};
    std::size_t used{};
};

std::string_view ExpressionName(OperandOp op) {
    switch (op) {
        case OperandOp::Identity:
            return "identity";
        case OperandOp::Add:
            return "add";
        case OperandOp::Sub:
            return "sub";
        case OperandOp::AddShift:
            return "add-shift";
        case OperandOp::Lsl:
            return "lsl";
        case OperandOp::Lsr:
            return "lsr";
        case OperandOp::Asr:
            return "asr";
        case OperandOp::AddExtend:
            return "add-extend";
    }
    return "unknown-expression";
}

void Cell(Text& text, const Program& program, CellRange::Iterator& at, CellRange::Iterator end) {
    if (at == end) {
        text.Append("<missing>");
        return;
    }
    const auto cell = *at++;
    const auto kind = cell.Kind();
    const auto view = cell.TypeView();
    if (kind == OperandKind::Lambda) {
        text.Print("lambda<{:#x}>(", cell.Payload());
        Cell(text, program, at, end);
        text.Append(")");
        return;
    }
    if (kind == OperandKind::Expression || kind == OperandKind::Arguments) {
        text.Append(kind == OperandKind::Arguments ? "args" : ExpressionName(cell.ExpressionOp()));
        if (kind == OperandKind::Expression && cell.ShiftAmount())
            text.Print("<shift={}>", cell.ShiftAmount());
        text.Append("(");
        for (unsigned i = 0; i < cell.Count(); ++i) {
            if (i) text.Append(", ");
            Cell(text, program, at, end);
            if (at == end && i + 1 < cell.Count()) {
                text.Append(", <missing>");
                break;
            }
        }
        text.Append(")");
        return;
    }
    switch (kind) {
        case OperandKind::Value:
            text.Print("%{}", cell.Payload());
            if (view != Type::Void)
                text.Print(":{}", TypeName(view));
            else if (cell.Payload() < program.Operations().size())
                text.Print(":{}", TypeName(program.Operations()[cell.Payload()].type));
            return;
        case OperandKind::Immediate:
            text.Print("#{:#x}:{}", cell.Payload(), TypeName(view));
            return;
        case OperandKind::Uniform:
            text.Print("uniform[{:#x}]:{}", cell.Payload(), TypeName(view));
            return;
        case OperandKind::Local:
            text.Print("local[{}]:{}", cell.Payload(), TypeName(view));
            return;
        case OperandKind::Flags:
            text.Print("flags<{:#x}>", cell.Payload());
            return;
        case OperandKind::Condition:
            text.Print("cond<{}>", cell.Payload());
            return;
        default:
            text.Print("<operand kind={} payload={:#x}>", unsigned(kind), cell.Payload());
            return;
    }
}

void OperationText(Text& text, const Program& program, NodeId node) {
    if (node.value >= program.Operations().size()) {
        text.Print("  %{} <unavailable>\n", node.value);
        return;
    }
    const auto& operation = program.Operations()[node.value];
    text.Print("  %{}:{} = {}", node.value, TypeName(operation.type), Name(operation.opcode));
    const auto operands = program.Operands(node);
    auto at = operands.begin();
    bool first = true;
    while (at != operands.end()) {
        text.Append(first ? " " : ", ");
        first = false;
        Cell(text, program, at, operands.end());
    }
    const auto metadata = operation.Metadata();
    if (metadata != OperationMetadata{})
        text.Print(" [flags={:#x} profile={} branch-only={} discard-omitted={}]",
                   metadata.flag_outputs,
                   unsigned(metadata.profile),
                   metadata.branch_only,
                   metadata.discard_omitted_flags);
    text.Append("\n");
}

void TerminalText(Text& text, const Program& program, const StoredTerminal& terminal) {
    const auto type = terminal.value_type == Type::Void && terminal.HasValue() &&
                                      terminal.value.value < program.Operations().size()
                              ? program.Operations()[terminal.value.value].type
                              : terminal.value_type;
    switch (terminal.kind) {
        case TerminalKind::Incomplete:
            text.Append("  incomplete\n");
            return;
        case TerminalKind::Return:
            if (terminal.HasValue())
                text.Print("  return %{}:{}\n", terminal.value.value, TypeName(type));
            else
                text.Append("  return\n");
            return;
        case TerminalKind::Jump:
            text.Print("  jump b{}\n", terminal.first.value);
            return;
        case TerminalKind::Branch:
            text.Print("  branch %{}:{} ? b{} : b{}\n",
                       terminal.value.value,
                       TypeName(type),
                       terminal.first.value,
                       terminal.second.value);
            return;
        case TerminalKind::Unreachable:
            text.Append("  unreachable\n");
            return;
        case TerminalKind::Dispatch:
            text.Append("  dispatch\n");
            return;
    }
}
}  // namespace

void FormatTo(FormatSink sink, const Program& program) {
    Text text{sink};
    if (!program)
        text.Append("program <consumed>\n");
    else {
        text.Print("program {} scope={} entry=b{} target={} isa={:#x} features={:#x}\n",
                   program.UnitId(),
                   program.ScopeKind() == Scope::Block ? "block" : "function",
                   program.EntryBlock().value,
                   unsigned(program.Target().mode),
                   program.Target().target,
                   program.Target().features);
        unsigned index{};
        for (const auto& block : program.Blocks()) {
            text.Print("b{}{} state={}:\n",
                       index,
                       index == program.EntryBlock().value ? " (entry)" : "",
                       unsigned(block.State()));
            for (unsigned n = block.first_node; n < block.first_node + block.node_count; ++n)
                OperationText(text, program, NodeId{n});
            TerminalText(text, program, block.terminal);
            ++index;
        }
    }
    text.Flush();
}

void FormatOperationTo(FormatSink sink, const Program& program, NodeId node) {
    Text text{sink};
    OperationText(text, program, node);
    text.Flush();
}

std::string ToString(const Program& program) {
    std::string result;
    FormatTo({&result,
              [](void* context, std::string_view text) {
                  static_cast<std::string*>(context)->append(text);
              }},
             program);
    return result;
}
}  // namespace swift::runtime::ir
