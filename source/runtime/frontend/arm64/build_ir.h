#pragma once
#include "region.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/frontend/build_result.h"
#include "runtime/ir/builder.h"

namespace swift::arm64 {
struct StateLayout {
    // Numeric leaves accept a canonical GuestState at gprs_offset; integer
    // uniform operations can also use separately placed register banks.
    std::uint32_t gprs_offset{}, pc_offset{256}, flags_offset{264}, fprs_offset{272};
    std::uint32_t fpcr_offset{784}, fpsr_offset{788}, tpidr_offset{792},
            numeric_control_offset{800};
    std::uint32_t helper_status_offset{804};
    std::uint32_t service_word_offset{808}, reservation_valid_offset{812};
};

using runtime::frontend::BuildError;
using runtime::frontend::BuildResult;
BuildResult BuildIR(std::span<const BlockWindow>,
                    runtime::ir::Scope,
                    StateLayout = {},
                    runtime::ir::TargetInfo = {},
                    runtime::ir::WorkCounts* = nullptr,
                    DecodeTiming* = nullptr,
                    bool external_edges = false,
                    runtime::ir::Program* reusable = nullptr,
                    std::vector<runtime::SourceSpan>* source_spans = nullptr,
                    std::vector<std::uint64_t>* continuations = nullptr);
BuildResult BuildDecodedIR(std::span<const Instruction>,
                           std::span<const DecodedBlock>,
                           std::span<const BlockWindow>,
                           runtime::ir::Scope,
                           StateLayout = {},
                           runtime::ir::TargetInfo = {},
                           runtime::ir::WorkCounts* = nullptr,
                           std::uint32_t entry_block = 0,
                           runtime::ir::Program* reusable = nullptr,
                           std::vector<runtime::SourceSpan>* source_spans = nullptr,
                           std::vector<std::uint64_t>* continuations = nullptr);
BuildResult BuildFunctionIR(std::span<const BlockWindow>,
                            std::uint64_t entry,
                            StateLayout = {},
                            DecodeLimits = {},
                            runtime::ir::TargetInfo = {},
                            runtime::ir::WorkCounts* = nullptr,
                            DecodeTiming* = nullptr,
                            runtime::ir::Program* reusable = nullptr,
                            ReadyEntryLookup = {},
                            std::vector<runtime::SourceSpan>* source_spans = nullptr,
                            std::vector<std::uint64_t>* continuations = nullptr);
}  // namespace swift::arm64
