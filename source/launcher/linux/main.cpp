#include "base/logging.h"
#include "options.h"
#include "report.h"

#include <csignal>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

extern char** environ;

int main(int argc, char** argv) {
    using namespace swift;
    log::SetSink(&log::StandardErrorSink());
    try {
        auto options = launcher::ParseOptions(argc, argv);
        options.runtime.forward_process_signals = true;
        if (options.help) {
            launcher::PrintUsage();
            return 0;
        }
        std::vector<std::string> arguments, environment;
        for (int i = options.guest_argument; i < argc; ++i) arguments.emplace_back(argv[i]);
        for (char** variable = environ; variable && *variable; ++variable)
            environment.emplace_back(*variable);
        const auto result =
                runtime::execution::RunGuest(arguments.empty() ? std::string{} : arguments.front(),
                                             arguments,
                                             environment,
                                             options.runtime);
        if (!result) SVM_LOG(Error, Runtime, "guest execution stopped: {}", result.error);
        launcher::WriteReport(stderr, result, options.runtime);
        if (result.reason == runtime::GuestStopReason::GuestSignalTermination) {
            // The VM and its activation have closed. The standalone process
            // now exposes the same terminating signal to its parent.
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            sigaction(result.termination_signal, &action, nullptr);
            sigset_t signal;
            sigemptyset(&signal);
            sigaddset(&signal, result.termination_signal);
            sigprocmask(SIG_UNBLOCK, &signal, nullptr);
            raise(result.termination_signal);
            return 128 + result.termination_signal;
        }
        // Keep the guest's exit value in the report even if host cleanup failed.
        return result ? result.exit_code : 125;
    } catch (const std::exception& error) {
        SVM_LOG(Error, Runtime, "runner error: {}", error.what());
        return 125;
    }
}
