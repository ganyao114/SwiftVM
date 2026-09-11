#include "function_region_decoder.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "runtime/common/perf_stats.h"
#include "runtime/frontend/x86/decoder.h"

namespace swift::translator::x86 {

using namespace runtime;

FunctionRegionDecoder::FunctionRegionDecoder(ir::HIRBuilder& builder,
                                             ir::HIRFunction& function,
                                             FunctionRegionDecodeConfig config)
        : builder(builder), function(function), config(std::move(config)), frontier(&function) {
    ASSERT(this->config.block_cap > 0 && this->config.memory);
    ASSERT(this->config.local_target && this->config.has_code);
}

LocationDescriptor FunctionRegionDecoder::NearestEntry(LocationDescriptor address, VAddr upper) {
    LocationDescriptor stop{};
    for (auto& candidate : function.GetHIRBlockList()) {
        const auto location = candidate.GetBlock()->GetStartLocation().Value();
        if (location > address && location < upper && (stop == 0 || location < stop)) {
            stop = location;
        }
    }
    return stop;
}

void FunctionRegionDecoder::DecodeBlock(LocationDescriptor address,
                                        ir::HIRBlock* block,
                                        LocationDescriptor stop,
                                        ::swift::x86::DecodeStopKind stop_kind) {
    builder.SetCurBlock(block);
    ir::Assembler assembler{&builder};
    ::swift::x86::X64Decoder decoder{
            address,
            config.memory,
            &assembler,
            true,
            config.arm64_features,
            config.sse_afp_nan,
            config.native_memory,
            config.features,
            stop,
            stop_kind,
    };
    decoder.Decode();
}

FunctionRegionDecodeResult FunctionRegionDecoder::Decode() {
    FunctionRegionDecodeResult result{};
    PerfScope perf_decode{GetPerfStats().decode_ns};
    while (result.decoded_count < config.block_cap) {
        bool replayed_split = false;
        for (auto& candidate : function.GetHIRBlockList()) {
            auto* block = candidate.GetBlock();
            if (!block->GetInstList().empty() || block->HasTerminal()) {
                continue;
            }
            const auto target = block->GetStartLocation().Value();
            if (!config.local_target(target)) {
                continue;
            }
            auto split = frontier.FindSplit(target);
            if (!split) {
                continue;
            }
            auto* owner = split->owner;
            const auto owner_start = owner->GetBlock()->GetStartLocation().Value();
            if (!builder.ResetDecodedBlock(owner)) {
                frontier.Reject(target, FunctionDecodeFrontier::Rejection::OwnerResetFailed);
                replayed_split = true;
                break;
            }
            DecodeBlock(owner_start,
                        owner,
                        target,
                        split->provenance->call_return_boundary
                                ? ::swift::x86::DecodeStopKind::CallReturn
                                : ::swift::x86::DecodeStopKind::Internal);
            if (FunctionDecodeFrontier::DecodedEnd(owner->GetBlock()) == target) {
                frontier.Accept(target);
            } else {
                frontier.Reject(target, FunctionDecodeFrontier::Rejection::BoundaryMismatch);
            }
            replayed_split = true;
            break;
        }
        if (replayed_split) {
            continue;
        }

        std::vector<LocationDescriptor> to_decode;
        for (auto& candidate : function.GetHIRBlockList()) {
            auto* block = candidate.GetBlock();
            if (!block->GetInstList().empty() || block->HasTerminal()) {
                continue;
            }
            const auto address = block->GetStartLocation().Value();
            // A block that already has published code is owned by another
            // object; absorbing it here would orphan that object and leave
            // inbound links dangling. Skip it as an external boundary in both
            // lazy and eager decode — under eager IsAccepted is never set, so
            // this degrades to the plain has_code guard.
            if (!frontier.IsAccepted(address) && config.has_code(address)) {
                continue;
            }
            if (config.local_target(address)) {
                to_decode.push_back(address);
            }
        }
        if (to_decode.empty()) {
            auto external_roots = frontier.DiscoverExternalRoots(3);
            for (const auto target :
                 frontier.DiscoverExternalRoots(1, FunctionDecodeFrontier::OwnerPolicy::Interior)) {
                if (std::find(external_roots.begin(), external_roots.end(), target) ==
                    external_roots.end()) {
                    external_roots.push_back(target);
                }
            }
            for (const auto target : external_roots) {
                function.CreateOrGetBlock(ir::Location{target});
            }
            if (!external_roots.empty()) {
                continue;
            }
            break;
        }
        for (const auto address : to_decode) {
            if (result.decoded_count == config.block_cap) {
                result.hit_block_cap = true;
                break;
            }
            auto* block = function.CreateOrGetBlock(address);
            PerfScope2 perf_decode_detail{GetPerfStats2().decode_total};
            DecodeBlock(address, block, NearestEntry(address, UINT64_MAX));
            const auto end = FunctionDecodeFrontier::DecodedEnd(block->GetBlock());
            if (const auto late_entry = NearestEntry(address, end);
                late_entry != 0 && builder.ResetDecodedBlock(block)) {
                DecodeBlock(address, block, late_entry);
            }
            ++result.decoded_count;
        }
    }

    for (auto& candidate : function.GetHIRBlockList()) {
        auto* block = candidate.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            result.hit_block_cap = true;
            const auto root = block->GetStartLocation().Value();
            if (config.local_target(root) && !config.has_code(root)) {
                result.pending_roots.push_back(root);
            }
        }
    }
    std::sort(result.pending_roots.begin(), result.pending_roots.end());
    result.pending_roots.erase(
            std::unique(result.pending_roots.begin(),
                        result.pending_roots.end()),
            result.pending_roots.end());
    PerfScope2 perf_ir_finalize{GetPerfStats2().ir_finalize};
    function.SetFunctionEntryProvenance(frontier.ExportProvenance());
    function.EndFunction();
    perf_ir_finalize.Stop();
    perf_decode.Stop();

    for (auto* hir_block : function.GetHIRBlocks()) {
        if (!hir_block) {
            continue;
        }
        auto* block = hir_block->GetBlock();
        if (block->GetInstList().empty()) {
            continue;
        }
        ++result.decoded_blocks;
        result.has_host_call |= std::any_of(
                block->GetInstList().begin(), block->GetInstList().end(), [](const ir::Inst& inst) {
                    return inst.GetOp() == ir::OpCode::CallLambda;
                });
    }
    return result;
}

}  // namespace swift::translator::x86
