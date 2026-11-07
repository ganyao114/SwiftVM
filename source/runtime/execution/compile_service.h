#pragma once

#include "runtime/backend/code.h"
#include "runtime/ir/program.h"

namespace swift::runtime::execution {
class VM;
enum class CompileFailure : unsigned char { None, Unsupported, InvalidInput };

// The one cold compilation path. It owns the real reusable Program; the bound
// host backend owns its one output buffer. No preparation/action IR is created.
class CompileService {
public:
    explicit CompileService(VM& vm) noexcept : vm(vm) {}

    backend::NativeCode Compile(ir::Scope);

    const backend::CompileResult* LastOutput() const noexcept { return last; }

    CompileFailure Failure() const noexcept { return failure; }

    void Release() noexcept;

private:
    VM& vm;
    ir::Program reusable;
    std::vector<std::uint64_t> optimizer_rows;
    backend::CompileResult* last{};
    CompileFailure failure{};
};
}  // namespace swift::runtime::execution
