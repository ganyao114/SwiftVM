#pragma once
#include <vector>
#include "runtime/backend/reg_alloc.h"
namespace swift::runtime::backend::arm64 {
class BlockAnalysisIndex;
struct GPRWidthCertificates {
    std::vector<u32> width, low32, reads;
};
GPRWidthCertificates VerifyGPRWidthElisions(ir::Block* block, const RegAlloc& allocation,
        const FeatureSet& features, const GPRSMask& uniforms, const BlockAnalysisIndex* analysis = nullptr);
}
