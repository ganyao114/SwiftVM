#pragma once

#include "runtime/compile/analysis/guest_state.h"

#include <functional>
#include <vector>

#include "function_decode_frontier.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/include/config.h"

namespace swift::runtime {
class MemoryInterface;
namespace ir {

}
}

namespace swift::translator::x86 {

struct FunctionRegionDecodeConfig {
    runtime::LocationDescriptor entry{};
    size_t block_cap{};
    bool lazy{};
    runtime::MemoryInterface* memory{};
    runtime::Arm64Features arm64_features{};
    bool sse_afp_nan{};
    bool native_memory{};
    runtime::FeatureSet features{};
    const runtime::compile::GuestStateLayout* guest_layout{};
    runtime::compile::BuilderCapabilities builder_target{};
    runtime::TsoMode tso_mode{runtime::TsoMode::Relaxed};
    VAddr decode_limit{};
    std::function<bool(runtime::LocationDescriptor)> local_target;
    std::function<bool(runtime::LocationDescriptor)> has_code;
};

struct FunctionRegionDecodeResult {
    size_t decoded_count{};
    size_t decoded_blocks{};
    bool hit_block_cap{};
    bool has_host_call{};
    std::vector<runtime::LocationDescriptor> pending_roots;
};

class FunctionRegionDecoder final {
public:
    FunctionRegionDecoder(runtime::ir::HIRBuilder& builder,
                          runtime::ir::HIRFunction& function,
                          FunctionRegionDecodeConfig config);

    [[nodiscard]] FunctionRegionDecodeResult Decode();

private:
    [[nodiscard]] runtime::LocationDescriptor NearestEntry(runtime::LocationDescriptor address,
                                                           VAddr upper);
    void DecodeBlock(
            runtime::LocationDescriptor address,
            const runtime::ir::HIRBlock* block,
            runtime::LocationDescriptor stop,
            ::swift::x86::DecodeStopKind stop_kind = ::swift::x86::DecodeStopKind::Internal);

    runtime::ir::HIRBuilder& builder;
    runtime::ir::HIRFunction& function;
    FunctionRegionDecodeConfig config;
    FunctionDecodeFrontier frontier;
};

}  // namespace swift::translator::x86
