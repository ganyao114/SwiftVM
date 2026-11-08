#include "guest_execution.h"

namespace swift::runtime::execution {
namespace {
std::recursive_mutex& ExecutionMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
}  // namespace

GuestExecutionLock::GuestExecutionLock() : lock(ExecutionMutex()) {}

}  // namespace swift::runtime::execution
