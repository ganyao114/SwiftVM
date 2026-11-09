#include "base/error.h"
#include "base/format.h"
#include "options.h"

#include <charconv>
#include <cstdio>
#include <string_view>

namespace swift::launcher {
namespace {
[[noreturn]] void Invalid(std::string message) {
    ThrowError(std::move(message), ErrorKind::InvalidArgument);
}
}  // namespace

Options ParseOptions(int argc, char* const* argv) {
    using namespace runtime::execution;
    Options parsed;
    auto& options = parsed.runtime;
    int argument = 1;
    const auto value = [&](std::string_view option) -> std::string_view {
        if (++argument == argc) Invalid(Format("{} requires a value", option));
        return argv[argument];
    };
    for (; argument < argc; ++argument) {
        const std::string_view option{argv[argument]};
        if (option == "--") {
            ++argument;
            break;
        }
        if (option == "--help") {
            parsed.help = true;
            return parsed;
        }
        if (option == "--scope") {
            const auto scope = value(option);
            if (scope != "block" && scope != "function")
                Invalid("--scope requires block or function");
            options.scope =
                    scope == "function" ? runtime::ir::Scope::Function : runtime::ir::Scope::Block;
        } else if (option == "--engine") {
            const auto engine = value(option);
            if (engine == "jit")
                options.mode = ExecutionMode::JitOnly;
            else if (engine == "interpreter")
                options.mode = ExecutionMode::InterpretOnly;
            else if (engine == "mixed")
                options.mode = ExecutionMode::Mixed;
            else
                Invalid("--engine requires jit, interpreter or mixed");
        } else if (option == "--hot-threshold") {
            const auto count = value(option);
            const auto number = std::from_chars(
                    count.data(), count.data() + count.size(), options.hot_threshold);
            if (number.ec != std::errc{} || number.ptr != count.data() + count.size())
                Invalid("--hot-threshold requires an integer in 0..65535");
        } else if (option == "--no-opt")
            options.optimize = false;
        else if (option == "--cache-dir")
            options.cache_directory = value(option);
        else if (option == "--aot")
            options.artifact_path = value(option);
        else if (option == "--decode-timing")
            options.decode_timing = true;
        else if (option == "--compile-details") {
            options.decode_timing = true;
            options.compile_details = true;
        } else if (option.starts_with('-'))
            Invalid(Format("unknown runner option: {}", option));
        else
            break;
    }
    if (argument >= argc && options.artifact_path.empty()) Invalid("a guest ELF path is required");
    parsed.guest_argument = argument;
    return parsed;
}

void PrintUsage() {
    std::puts(
            "svm_rebuild_linux [--scope block|function] [--engine jit|interpreter|mixed] "
            "[--hot-threshold N] [--no-opt] [--decode-timing] [--compile-details] [--] guest "
            "[args...]\n"
            "  [--cache-dir DIR] persists native code across processes (off by default).\n"
            "  [--aot ARTIFACT] loads embedded guest/code; the guest path is optional.\n"
            "Static ARM32 ELF32, x86-64 or AArch64 ELF64, direct guest==host memory, one guest "
            "thread.\n"
            "PT_INTERP and unknown instructions are rejected. Unimplemented syscalls return "
            "ENOSYS.\n"
            "Default engine: jit; compile each requested entry immediately.\n"
            "With --engine mixed, interpret cold blocks and compile after N entry visits "
            "(--hot-threshold, default 8).\n"
            "--decode-timing adds perturbed diagnostic clocks around the byte decoder only.\n"
            "--compile-details also measures formation, IR construction and publication stages.");
}
}  // namespace swift::launcher
