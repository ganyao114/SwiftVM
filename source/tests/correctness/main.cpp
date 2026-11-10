#include "case.h"
#include "catalog.h"
#include "execute.h"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include "base/error.h"
#include "base/format.h"
#include "runtime/arch/arm32/location.h"
#include "runtime/frontend/arm32/decode.h"
#include "runtime/frontend/arm64/decode.h"
#include "runtime/frontend/x86/decode.h"

extern char** environ;

namespace swift::tests::correctness {
namespace {
namespace fs = std::filesystem;

class Arguments {
public:
    Arguments(int argc, char** argv) {
        for (int i = 2; i < argc; ++i) values.emplace_back(argv[i]);
    }
    bool Has(std::string_view key) const {
        return std::find(values.begin(), values.end(), key) != values.end();
    }

    std::string Get(std::string_view key, std::string fallback = {}) const {
        const auto it = std::find(values.begin(), values.end(), key);
        if (it == values.end()) return fallback;
        if (it + 1 == values.end() || (it + 1)->starts_with("--"))
            ThrowError(Format("missing value for {}", key), ErrorKind::InvalidArgument);
        return *(it + 1);
    }

    std::uint64_t Number(std::string_view key, std::uint64_t fallback) const {
        const auto value = Get(key);
        if (value.empty()) return fallback;
        std::size_t end{};
        const auto result = std::stoull(value, &end, 0);
        if (end != value.size()) ThrowError("invalid numeric argument", ErrorKind::InvalidArgument);
        return result;
    }

private:
    std::vector<std::string> values;
};

void WriteText(const std::string& path, const std::string& content) {
    if (path.empty()) {
        std::cout << content << '\n';
        return;
    }
    const auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << content << '\n';
    if (!stream) ThrowError(Format("cannot write {}", path), ErrorKind::System);
}

RunConfig Config(const Arguments& args) {
    RunConfig config;
    if (args.Has("--config")) config = ReadConfig(args.Get("--config"));
    if (args.Has("--backend")) config.backend = ParseBackend(args.Get("--backend"));
    if (args.Has("--scope")) {
        const auto scope = args.Get("--scope");
        if (scope != "block" && scope != "function")
            ThrowError("scope must be block or function", ErrorKind::InvalidArgument);
        config.scope = scope == "function" ? Scope::Function : Scope::Block;
    }
    if (args.Has("--target")) {
        const auto target = args.Get("--target");
        if (target != "known" && target != "early-off" && target != "unknown")
            ThrowError("target must be known, early-off or unknown", ErrorKind::InvalidArgument);
        config.target = target == "known"       ? TargetMode::Known
                        : target == "early-off" ? TargetMode::EarlyOff
                                                  : TargetMode::Unknown;
    }
    if (args.Has("--no-opt")) config.optimize = false;
    if (args.Has("--no-pin")) config.pin = false;
    if (args.Has("--compile-only")) config.compile_only = true;
    return config;
}

Observation Execute(const Case& c, const RunConfig& config) {
    if (config.compile_only) return ExecuteCompile(c, config);
    return config.backend == Backend::Direct ? ExecuteDirect(c) : ExecuteVM(c, config);
}

void RunWorker(const std::string& input, const std::string& output, const RunConfig& config) {
    Observation observation;
    observation.outcome = Outcome::HostFailure;
    observation.stage = "worker";
    observation.backend = Name(config.backend);
    try {
        const auto c = ReadCase(input);
        std::string reason;
        if (!ValidateCase(c, &reason))
            ThrowError(Format("case does not match its contract: {}", reason),
                       ErrorKind::InvalidArgument);
        observation = Execute(c, config);
    } catch (const std::exception& error) {
        observation.error = error.what();
    } catch (...) {
        observation.error = "unknown worker failure";
    }
    WriteText(output, ToJSON(observation));
}

int Worker(const Arguments& args) {
    const auto config = Config(args);
    if (!args.Has("--input-dir")) {
        RunWorker(args.Get("--case"), args.Get("--out"), config);
        return 0;
    }
    if (!args.Has("--output-dir"))
        ThrowError("batch worker requires --output-dir", ErrorKind::InvalidArgument);
    const fs::path output = args.Get("--output-dir");
    fs::create_directories(output);
    std::vector<fs::path> cases;
    for (const auto& file : fs::directory_iterator(args.Get("--input-dir")))
        if (file.is_regular_file() && file.path().extension() == ".svmcase")
            cases.push_back(file.path());
    std::sort(cases.begin(), cases.end());
    for (const auto& input : cases) {
        // A crash leaves the precise in-flight case and completed prefix.
        WriteText((output / "current.json").string(),
                  Format("{{\"case\":\"{}\",\"config\":{}}}",
                         input.filename().string(),
                         ToJSON(config)));
        RunWorker(input.string(), (output / (input.stem().string() + ".json")).string(), config);
    }
    fs::remove(output / "current.json");
    return 0;
}

std::vector<const Form*> Selected(const Arguments& args) {
    std::vector<const Form*> forms;
    const auto guest = args.Get("--guest");
    const auto id = args.Get("--form");
    for (const auto& form : Catalog())
        if ((guest.empty() || form.isa == ParseISA(guest)) && (id.empty() || form.id == id))
            forms.push_back(&form);
    if (forms.empty()) ThrowError("no matching instruction forms", ErrorKind::InvalidArgument);
    return forms;
}

int GenerateCases(const Arguments& args) {
    const auto forms = Selected(args);
    const auto seed = args.Number("--seed", 1);
    const auto first = args.Number("--first-id", 0);
    const auto per_form = args.Number("--cases-per-form", 0);
    const auto count = per_form ? per_form * forms.size() : args.Number("--cases", forms.size());
    const fs::path out = args.Get("--out", "corpus");
    fs::create_directories(out);
    for (std::uint64_t i = first; i - first < count; ++i) {
        const auto c = Generate(forms[i % forms.size()]->id, seed, per_form ? i / forms.size() : i);
        WriteCase((out / Format("{:016x}.svmcase", i)).string(), c);
    }
    std::cout << Format("{{\"cases\":{},\"forms\":{},\"seed\":{}}}\n", count, forms.size(), seed);
    return 0;
}

int CompareFiles(const Arguments& args) {
    const auto comparison = Compare(ReadCase(args.Get("--case")),
                                    ReadObservation(args.Get("--expected")),
                                    ReadObservation(args.Get("--actual")));
    WriteText(args.Get("--out"), ToJSON(comparison));
    return !comparison.comparable ? 77 : comparison.equal ? 0 : 1;
}

int ReduceFile(const Arguments& args) {
    const auto input = ReadCase(args.Get("--case"));
    const auto output = args.Get("--out");
    const auto executable = args.Get("--predicate");
    if (output.empty() || executable.empty())
        ThrowError("reduce requires --out and --predicate executable", ErrorKind::InvalidArgument);
    const auto parent = fs::path(output).parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    const auto candidate = output + Format(".candidate-{}.svmcase", getpid());
    const auto predicate = [&](const Case& c) {
        WriteCase(candidate, c);
        std::array<char*, 3> argv{const_cast<char*>(executable.c_str()),
                                  const_cast<char*>(candidate.c_str()),
                                  nullptr};
        pid_t pid{};
        if (posix_spawnp(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ))
            ThrowError("cannot start reduce predicate", ErrorKind::System);
        int status{};
        while (waitpid(pid, &status, 0) < 0)
            if (errno != EINTR) ThrowError("cannot wait for reduce predicate", ErrorKind::System);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    };
    if (!predicate(input))
        ThrowError("original failure predicate did not reproduce", ErrorKind::InvalidState);
    const auto reduced = Reduce(input, predicate, args.Number("--budget", 256));
    WriteCase(output, reduced);
    fs::remove(candidate);
    std::cout << ToJSON(reduced) << '\n';
    return 0;
}

int SelfTest() {
    std::size_t cases{};
    for (const auto& form : Catalog())
        for (std::uint64_t i = 0; i != 16; ++i) {
            const auto c = Generate(form.id, 0xc0de, i);
            std::string reason;
            if (!ValidateCase(c, &reason)) ThrowError(reason, ErrorKind::InvalidState);
            if (CaseHash(c) != CaseHash(Decode(Encode(c))))
                ThrowError("case codec roundtrip mismatch", ErrorKind::InvalidState);
            Observation expected;
            expected.outcome = Outcome::Terminal;
            expected.state = c.initial;
            expected.state.pc = c.terminal_pc;
            auto actual = expected;
            if (!Compare(c, expected, actual).equal)
                ThrowError("identical observations compare unequal", ErrorKind::InvalidState);
            actual.state.gpr[0] ^= 1;
            if (Compare(c, expected, actual).equal)
                ThrowError("comparator lost a GPR difference", ErrorKind::InvalidState);
            actual = expected;
            actual.outcome = Outcome::Compiled;
            if (Compare(c, expected, actual).equal)
                ThrowError("compile-only was counted as execution", ErrorKind::InvalidState);
            if (c.isa == ISA::X86_64 && (Contract(c).fp_control_mask & 2)) {
                actual = expected;
                actual.state.fp_control ^= 2;
                if (Compare(c, expected, actual).equal)
                    ThrowError("comparator lost an MXCSR DE difference", ErrorKind::InvalidState);
                auto masked = expected;
                masked.fp_control_verified_mask &= ~std::uint32_t{2};
                if (!Compare(c, masked, actual).equal)
                    ThrowError("oracle capability mask was ignored", ErrorKind::InvalidState);
            }
            ++cases;
        }
    std::cout << Format("{{\"selftest\":\"passed\",\"cases\":{}}}\n", cases);
    return 0;
}

}  // namespace

std::vector<std::uint8_t> ParseHex(std::string_view text) {
    if (text.empty() || text.size() % 2)
        ThrowError("--code must be an even-length hex string", ErrorKind::InvalidArgument);
    std::vector<std::uint8_t> bytes;
    bytes.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned value{};
        const auto parsed = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + i + 2)
            ThrowError("invalid hex byte", ErrorKind::InvalidArgument);
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

int DecodeCommand(const Arguments& args) {
    const auto isa = ParseISA(args.Get("--isa"));
    const auto code = ParseHex(args.Get("--code"));
    const auto pc = args.Number("--pc", 0x04000000);
    const std::span<const std::uint8_t> bytes{code};
    if (isa == ISA::X86_64) {
        const auto decoded = swift::x86::DecodeOne(bytes, pc);
        if (!decoded) {
            std::cout << "{\"ok\":false}\n";
            return 1;
        }
        std::cout << Format("{{\"ok\":true,\"isa\":\"x86_64\",\"size\":{},\"op\":{},\"operands\":[",
                            decoded.instruction.View().Size(),
                            unsigned(decoded.instruction.View().Opcode()));
        const auto& record = decoded.instruction.View().Record();
        for (unsigned i = 0; i != record.ops.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << Format("{{\"kind\":{},\"index\":{},\"size\":{}}}",
                                unsigned(record.ops[i].type),
                                unsigned(record.ops[i].index),
                                unsigned(record.ops[i].size));
        }
        std::cout << "]}\n";
        return 0;
    }
    if (isa == ISA::Arm64) {
        const auto decoded = swift::arm64::DecodeOne(bytes, pc);
        if (!decoded) {
            std::cout << "{\"ok\":false}\n";
            return 1;
        }
        std::cout << Format("{{\"ok\":true,\"isa\":\"arm64\",\"family\":{},\"word\":{}}}\n",
                            decoded.instruction.FamilyCode(),
                            decoded.instruction.Word());
        return 0;
    }
    const auto location = swift::runtime::arm32::Location::Make(
            static_cast<std::uint32_t>(pc), args.Has("--thumb"), 0, 0);
    const auto decoded = swift::arm32::DecodeOne(bytes, location.Key());
    if (!decoded) {
        std::cout << "{\"ok\":false}\n";
        return 1;
    }
    std::cout << Format(
            "{{\"ok\":true,\"isa\":\"arm32\",\"length\":{},\"opcode\":{},\"name\":\"{}\","
            "\"word\":{}}}\n",
            decoded.instruction.Length(),
            decoded.instruction.OpcodeCode(),
            decoded.instruction.Name(),
            decoded.instruction.Word());
    return 0;
}

int Main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: svm_correctness "
                 "{catalog|generate|worker|replay|compare|reduce|inspect|decode|selftest}\n";
        return 2;
    }
    const std::string_view command = argv[1];
    const Arguments args{argc, argv};
    if (command == "worker" || command == "replay") return Worker(args);
    if (command == "generate") return GenerateCases(args);
    if (command == "compare") return CompareFiles(args);
    if (command == "reduce") return ReduceFile(args);
    if (command == "selftest") return SelfTest();
    if (command == "decode") return DecodeCommand(args);
    if (command == "inspect") {
        WriteText(args.Get("--out"), ToJSON(ReadCase(args.Get("--case"))));
        return 0;
    }
    if (command == "catalog") {
        const auto forms = Selected(args);
        std::cout << "{\"schema_version\":1,\"native_backend\":\"" << Name(NativeBackend())
                  << "\",\"forms\":[";
        bool first = true;
        for (const auto* form : forms) {
            if (!first) std::cout << ',';
            first = false;
            std::cout << Format("{{\"id\":\"{}\",\"isa\":\"{}\"}}", form->id, Name(form->isa));
        }
        std::cout << "]}\n";
        return 0;
    }
    ThrowError("unknown correctness command", ErrorKind::InvalidArgument);
}

}  // namespace swift::tests::correctness

int main(int argc, char** argv) {
    try {
        return swift::tests::correctness::Main(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "svm_correctness: " << error.what() << '\n';
        return 2;
    }
}
