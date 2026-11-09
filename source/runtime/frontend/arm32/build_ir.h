#pragma once

#include "region.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/frontend/build_result.h"
#include "runtime/ir/builder.h"

namespace swift::arm32 {
struct StateLayout {
    u32 gprs_offset{}, psr_aux_offset{120}, pc_offset{128}, flags_offset{136}, fprs_offset{144};
    u32 fpscr_offset{400}, numeric_control_offset{404}, tpidrurw_offset{408}, tpidruro_offset{412};
    u32 service_word_offset{416}, helper_status_offset{420}, reservation_valid_offset{424};
    u32 service_pc_offset{432};
};

using runtime::frontend::BuildError;
using runtime::frontend::BuildResult;
BuildResult BuildIR(std::span<const BlockWindow>,
                    u64 entry,
                    runtime::ir::Scope,
                    StateLayout = {},
                    runtime::ir::TargetInfo = {},
                    runtime::ir::WorkCounts* = nullptr,
                    DecodeTiming* = nullptr,
                    bool external_edges = false,
                    runtime::ir::Program* reusable = nullptr,
                    std::vector<runtime::SourceSpan>* = nullptr,
                    std::vector<u64>* = nullptr,
                    Profile = {});
BuildResult BuildIR(std::span<const BlockWindow>,
                    runtime::ir::Scope,
                    StateLayout = {},
                    runtime::ir::TargetInfo = {},
                    runtime::ir::WorkCounts* = nullptr,
                    DecodeTiming* = nullptr,
                    bool external_edges = false,
                    runtime::ir::Program* reusable = nullptr,
                    std::vector<runtime::SourceSpan>* = nullptr,
                    std::vector<u64>* = nullptr,
                    Profile = {});
BuildResult BuildDecodedIR(std::span<const Instruction>,
                           std::span<const DecodedBlock>,
                           std::span<const BlockWindow>,
                           runtime::ir::Scope,
                           StateLayout = {},
                           runtime::ir::TargetInfo = {},
                           runtime::ir::WorkCounts* = nullptr,
                           u32 entry_block = 0,
                           runtime::ir::Program* reusable = nullptr,
                           std::vector<runtime::SourceSpan>* = nullptr,
                           std::vector<u64>* = nullptr,
                           Profile = {});
BuildResult BuildFunctionIR(std::span<const BlockWindow>,
                            u64 entry,
                            StateLayout = {},
                            DecodeLimits = {},
                            runtime::ir::TargetInfo = {},
                            runtime::ir::WorkCounts* = nullptr,
                            DecodeTiming* = nullptr,
                            runtime::ir::Program* reusable = nullptr,
                            ReadyEntryLookup = {},
                            std::vector<runtime::SourceSpan>* = nullptr,
                            std::vector<u64>* = nullptr,
                            Profile = {});
}  // namespace swift::arm32
