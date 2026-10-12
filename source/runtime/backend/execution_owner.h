#pragma once

namespace swift::runtime::backend {

// Internal wrapper admission check. Setters and direct State access remain
// stopped-owner operations; this helper does not grant ownership.
bool RuntimeExecutionActiveOnCurrentThread() noexcept;

} // namespace swift::runtime::backend
