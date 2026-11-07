#pragma once

#include "runtime/backend/code.h"
#include "runtime/ir/program.h"
#include "runtime/frontend/x86/build_ir.h"
#include <string>

namespace swift::runtime::execution {
class VM;
struct CodeUnit;
enum class CompileFailure : unsigned char { None, Unsupported, InvalidInput };

enum class CompileStage : std::uint8_t { Source, Frontend, Optimize, Backend };

// One failed request retains actual source facts; formatting occurs only when
// the VM reports a terminal failure, not during a successful fallback.
struct CompileDiagnostic {
    CompileStage stage{};
    ir::Scope scope{};
    std::uint64_t pc{};
    ::swift::x86::BuildError frontend_error{};
    ir::Error ir_error{};
    backend::CompileError backend_error{};
    ir::NodeId node{};
    ir::BlockId block{};
    ir::OpCode opcode{};
    bool has_opcode{};
    std::size_t instructions{}, bytes{};
    const char* message{};
};

// The one cold compilation path. It owns the real reusable Program; the bound
// host backend owns its one output buffer. No preparation/action IR is created.
class CompileService {
public:
    explicit CompileService(VM& vm) noexcept : vm(vm) {}

    backend::NativeCode Compile(ir::Scope);
    backend::NativeCode Restore(const CodeUnit&, bool sources_verified = false);

    std::span<const std::uint64_t> Continuations() const noexcept { return continuations; }

    const backend::CompileResult* LastOutput() const noexcept { return last; }

    std::uint64_t EntryPC() const noexcept { return entry_pc; }

    CompileFailure Failure() const noexcept { return failure; }

    const CompileDiagnostic& Diagnostic() const noexcept { return diagnostic; }

    std::string DescribeFailure() const;
    void Release() noexcept;

private:
    VM& vm;
    ir::Program reusable;
    std::vector<std::uint64_t> optimizer_rows;
    const backend::CompileResult* last{};
    std::uint64_t entry_pc{};
    std::vector<SourceSpan> source_spans;
    std::vector<std::uint64_t> continuations;
    CompileFailure failure{};
    CompileDiagnostic diagnostic;
    backend::NativeCode Publish(const backend::CompileResult&, std::vector<backend::FaultSite>);
};
}  // namespace swift::runtime::execution
