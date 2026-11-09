#pragma once

namespace swift::runtime::execution {
class GuestMemory;
}

namespace swift::runtime::platform {
void InstallArm32UserHelpers(execution::GuestMemory&);
}
