#include "options.h"
#include "report.h"
#include "base/logging.h"

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

extern char** environ;

int main(int argc, char** argv) {
    using namespace swift;
    log::SetSink(&log::StandardErrorSink());
    try {
        const auto options = launcher::ParseOptions(argc, argv);
        if (options.help) {
            launcher::PrintUsage();
            return 0;
        }
        std::vector<std::string> arguments, environment;
        for (int i = options.guest_argument; i < argc; ++i) arguments.emplace_back(argv[i]);
        for (char** variable = environ; variable && *variable; ++variable)
            environment.emplace_back(*variable);
        const auto result = runtime::execution::RunGuest(
                arguments.front(), arguments, environment, options.runtime);
        if (!result) SVM_LOG(Error, Runtime, "guest execution stopped: {}", result.error);
        launcher::WriteReport(stderr, result, options.runtime);
        // Keep the guest's exit value in the report even if host cleanup failed.
        return result ? result.exit_code : 125;
    } catch (const std::exception& error) {
        SVM_LOG(Error, Runtime, "runner error: {}", error.what());
        return 125;
    }
}
