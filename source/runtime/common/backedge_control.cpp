#include "backedge_control.h"

#include "runtime/common/svm_config.h"

namespace swift::runtime {

bool BackedgeLatchEnabled() {
    return GetSvmConfig().backedge_latch;
}

bool BackedgeFlagsEnabled() {
    return BackedgeLatchEnabled() && GetSvmConfig().backedge_flags;
}

}  // namespace swift::runtime
