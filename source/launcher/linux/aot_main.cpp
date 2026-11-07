#include "aot/artifact.h"
#include "aot/call.h"
#include "aot/compile.h"
#include "base/error.h"
#include "base/logging.h"
#include "options.h"
#include "report.h"

#include <array>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <vector>

extern char** environ;

namespace {
using namespace swift;
using U64 = std::uint64_t;

U64 Number(std::string_view input) {
    U64 result{};
    int base = 10;
    if (input.starts_with("0x")) {
        input.remove_prefix(2);
        base = 16;
    }
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result, base);
    if (input.empty() || parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
        ThrowError("expected an unsigned decimal or hexadecimal integer",
                   ErrorKind::InvalidArgument);
    return result;
}

void Usage() {
    std::puts(
            "svm_aot compile guest.elf -o output.aot [--scope block|function] [--entry PC]\n"
            "    [--no-symbols] [--no-sweep] [--max N] [--no-opt] [--decode-timing]\n"
            "svm_aot run [--aot artifact.aot] [runner options] [--] [guest [args...]]\n"
            "svm_aot info artifact.aot\n"
            "svm_aot patch guest.elf stop_symbol -o patched.elf\n"
            "svm_aot call artifact.aot symbol [--no-opt] [--decode-timing] [--startup stop_symbol] "
            "[u64 arguments, at most "
            "8]\n"
            "The C++ GuestFn API also supports FP, aggregates, pointers and variadic calls.\n"
            "For function artifacts use --scope function when running. Artifacts carry the guest "
            "ELF.");
}

int Compile(int argc, char** argv) {
    if (argc < 3) ThrowError("compile requires a guest ELF", ErrorKind::InvalidArgument);
    aot::CompileOptions options;
    options.guest_elf = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string_view option{argv[i]};
        const auto value = [&]() -> std::string_view {
            if (++i == argc) ThrowError("AOT option requires a value", ErrorKind::InvalidArgument);
            return argv[i];
        };
        if (option == "-o")
            options.output = value();
        else if (option == "--scope") {
            const auto scope = value();
            if (scope != "block" && scope != "function")
                ThrowError("--scope requires block or function", ErrorKind::InvalidArgument);
            options.runtime.scope =
                    scope == "function" ? runtime::ir::Scope::Function : runtime::ir::Scope::Block;
        } else if (option == "--entry")
            options.entries.push_back(Number(value()));
        else if (option == "--max") {
            const auto count = Number(value());
            if (!count || count > UINT32_MAX)
                ThrowError("--max requires 1..4294967295", ErrorKind::InvalidArgument);
            options.max_entries = static_cast<std::uint32_t>(count);
        } else if (option == "--no-symbols")
            options.symbols = false;
        else if (option == "--no-sweep")
            options.sweep = false;
        else if (option == "--sweep")
            options.sweep = true;
        else if (option == "--no-opt")
            options.runtime.optimize = false;
        else if (option == "--decode-timing")
            options.runtime.decode_timing = true;
        else
            ThrowError("unknown AOT compile option", ErrorKind::InvalidArgument);
    }
    if (options.output.empty())
        ThrowError("compile requires -o output.aot", ErrorKind::InvalidArgument);
    aot::ArtifactStats stats;
    std::string error;
    if (!aot::CompileArtifact(options, stats, error)) ThrowError(std::move(error));
    std::printf(
            "{\"candidates\":%llu,\"units\":%llu,\"rejected\":%llu,\"functions\":%llu,\"stubbed\":%"
            "llu}\n",
            static_cast<unsigned long long>(stats.candidates),
            static_cast<unsigned long long>(stats.compiled_units),
            static_cast<unsigned long long>(stats.rejected),
            static_cast<unsigned long long>(stats.functions),
            static_cast<unsigned long long>(stats.stubbed));
    return 0;
}

int Run(int argc, char** argv) {
    // The same parser/report/run service as the ordinary launcher. --guest is
    // retained for the former svm_aot command line.
    std::vector<char*> arguments{argv[1]};
    for (int i = 2; i < argc; ++i)
        if (std::string_view{argv[i]} != "--guest") arguments.push_back(argv[i]);
    const auto options =
            launcher::ParseOptions(static_cast<int>(arguments.size()), arguments.data());
    if (options.help) {
        launcher::PrintUsage();
        return 0;
    }
    std::vector<std::string> guest, environment;
    for (std::size_t i = static_cast<std::size_t>(options.guest_argument); i < arguments.size();
         ++i)
        guest.emplace_back(arguments[i]);
    for (char** item = environ; item && *item; ++item) environment.emplace_back(*item);
    const auto result = runtime::execution::RunGuest(
            guest.empty() ? std::string{} : guest.front(), guest, environment, options.runtime);
    launcher::WriteReport(stderr, result, options.runtime);
    if (!result) SVM_LOG(Error, Runtime, "AOT run stopped: {}", result.error);
    return result ? result.exit_code : 125;
}

aot::Artifact Read(const char* path) {
    aot::Artifact result;
    std::string error;
    if (!aot::ReadArtifact(path, result, error)) ThrowError(std::move(error));
    return result;
}

int Info(const char* path) {
    const auto artifact = Read(path);
    std::printf(
            "AOT format: %u\nGuest: %s\nHost ISA: %#x\nScope: %s\n"
            "Units: %zu\nFunctions: %llu\nStubbed: %llu\nGuest digest: %s\n",
            aot::ArtifactVersion,
            artifact.guest_path.c_str(),
            artifact.code.key.host_isa,
            artifact.code.key.requested_scope == runtime::ir::Scope::Function ? "function"
                                                                              : "block",
            artifact.code.units.size(),
            static_cast<unsigned long long>(artifact.stats.functions),
            static_cast<unsigned long long>(artifact.stats.stubbed),
            HexDigest(artifact.code.key.guest_id).c_str());
    return 0;
}

template <std::size_t... I> U64 Invoke(aot::AotGuestEnv& env,
                                       const std::string& symbol,
                                       std::span<const U64> args,
                                       std::index_sequence<I...>) {
    using Signature = U64(decltype((void)I, U64{})...);
    return env.Lookup<Signature>(symbol)(args[I]...);
}

template <std::size_t Count>
U64 InvokeCount(aot::AotGuestEnv& env, const std::string& symbol, std::span<const U64> args) {
    return Invoke(env, symbol, args, std::make_index_sequence<Count>{});
}

template <std::size_t... I> constexpr auto Invokers(std::index_sequence<I...>) {
    return std::array{&InvokeCount<I>...};
}

int Call(int argc, char** argv) {
    if (argc < 4)
        ThrowError("call requires artifact, symbol and at most 8 arguments",
                   ErrorKind::InvalidArgument);
    std::vector<U64> args;
    runtime::execution::RuntimeOptions policy;
    std::string startup;
    for (int i = 4; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--no-opt")
            policy.optimize = false;
        else if (option == "--decode-timing")
            policy.decode_timing = true;
        else if (option == "--startup") {
            if (++i == argc)
                ThrowError("--startup requires a stop symbol", ErrorKind::InvalidArgument);
            startup = argv[i];
        } else
            args.push_back(Number(option));
    }
    if (args.size() > 8)
        ThrowError("call accepts at most 8 integer arguments", ErrorKind::InvalidArgument);
    aot::AotGuestEnv env;
    std::string error;
    if (!env.Init(argv[2], error, policy)) ThrowError(std::move(error));
    policy.scope = env.Image().code.key.requested_scope;
    if (!startup.empty() && !env.RunStartupUntil(startup, error)) ThrowError(std::move(error));
    if (!env.LookupSymbol(argv[3]))
        ThrowError("guest symbol was not found", ErrorKind::InvalidArgument);
    constexpr auto invoke = Invokers(std::make_index_sequence<9>{});
    const auto value = invoke[args.size()](env, argv[3], args);
    const auto released = env.Close();
    if (!released.Complete()) ThrowError("guest call mapping release failed");
    std::printf("%llu\n", static_cast<unsigned long long>(value));
    const runtime::execution::RunResult report{0, env.Stats(), {}, {}};
    launcher::WriteReport(stderr, report, policy);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    swift::log::SetSink(&swift::log::StandardErrorSink());
    try {
        if (argc < 2 || std::string_view{argv[1]} == "--help") {
            Usage();
            return argc < 2 ? 2 : 0;
        }
        const std::string_view command{argv[1]};
        if (command == "compile") return Compile(argc, argv);
        if (command == "run") return Run(argc, argv);
        if (command == "info" && argc == 3) return Info(argv[2]);
        if (command == "patch" && argc == 6 && std::string_view{argv[4]} == "-o") {
            std::string error;
            if (!swift::aot::PatchGuestStopByte(argv[2], argv[3], argv[5], error))
                swift::ThrowError(std::move(error));
            return 0;
        }
        if (command == "call") return Call(argc, argv);
        swift::ThrowError("unknown AOT command or invalid argument count",
                          swift::ErrorKind::InvalidArgument);
    } catch (const std::exception& error) {
        SVM_LOG(Error, Runtime, "AOT tool: {}", error.what());
        return 125;
    }
}
