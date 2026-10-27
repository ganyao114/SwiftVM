#pragma once

#include "certificates.h"

namespace swift::runtime::ir {
class HIRFunction;
}
namespace swift::runtime::backend::arm64 {
// Optional independent type/location/dominance/clobber audit. It never creates
// required certificates or prepares semantic facts.
void AuditGPRAllocation(std::span<const EmissionCertificateInput>,
                        compile::AllocationView,
                        const UnitActions&,
                        const FeatureSet&,
                        const ir::HIRFunction* function = nullptr,
                        bool internal_edges = true);
}  // namespace swift::runtime::backend::arm64
