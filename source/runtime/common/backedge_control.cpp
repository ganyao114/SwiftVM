#include "backedge_control.h"

#include <cstring>
#include "runtime/common/perf_stats.h"

namespace swift::runtime {

namespace {

bool EnvEnabled(const char* name) {
    const char* value = PerfGetenv(name);
    return value && std::strcmp(value, "0") != 0;
}

}  // namespace

bool BackedgeLatchEnabled() {
    static const bool enabled = EnvEnabled("SVM_BACKEDGE_LATCH");
    return enabled;
}

bool BackedgeFlagsEnabled() {
    static const bool enabled =
            BackedgeLatchEnabled() && EnvEnabled("SVM_BACKEDGE_FLAGS");
    return enabled;
}

}  // namespace swift::runtime
