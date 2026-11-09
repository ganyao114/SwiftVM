#pragma once

#include <signal.h>

namespace swift::runtime::platform {

// Called in ordinary context before exposing a guest disposition. Process
// handlers and their original actions remain alive until process exit.
bool EnsureManagedSignal(int signal) noexcept;

// Standalone process policy, called from ordinary runtime context. Resume
// after SIGCONT with the managed action and this signal's mask restored.
bool StopGuestProcess(int signal) noexcept;

// Embedders that already own host signals can explicitly route unclaimed
// events. Configure before entering a VM; this is not a guest handler.
using HostSignalBroker = bool (*)(int, siginfo_t*, void*) noexcept;
void SetHostSignalBroker(HostSignalBroker) noexcept;

}  // namespace swift::runtime::platform
