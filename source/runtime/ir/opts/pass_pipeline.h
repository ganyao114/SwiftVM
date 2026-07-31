//
// Created by SwiftVM on 2024/7/1.
//

#pragma once

#include <functional>
#include <vector>
#include "base/common_funcs.h"
#include "base/types.h"
#include "runtime/common/perf_stats.h"
#include "runtime/include/config.h"
#include "runtime/ir/block.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::ir {

struct UniformInfo;

// A single optimization pass entry
struct PassEntry {
    using BlockPassFn = std::function<void(Block*)>;
    using FunctionPassFn = std::function<void(HIRFunction*)>;

    Optimizations required_opt{Optimizations::None};
    BlockPassFn block_pass{};
    FunctionPassFn function_pass{};
};

// Manages and executes optimization passes in a defined order
class PassPipeline {
public:
    PassPipeline() = default;

    void AddBlockPass(Optimizations opt, PassEntry::BlockPassFn pass) {
        entries.push_back({opt, std::move(pass), {}});
    }

    void AddFunctionPass(Optimizations opt, PassEntry::FunctionPassFn pass) {
        entries.push_back({opt, {}, std::move(pass)});
    }

    void RunBlock(Block* block, Optimizations enabled_opts) const {
        PerfScope2 perf_total{GetPerfStats2().pass_total};
        for (auto& entry : entries) {
            if (!entry.block_pass) continue;
            if (entry.required_opt == Optimizations::None || True(enabled_opts & entry.required_opt)) {
                PerfScope2 perf_pass{CounterFor(entry.required_opt)};
                entry.block_pass(block);
            }
        }
        perf_total.Stop();
        // Passes may remove instructions, leaving non-dense ids. Re-id so that
        // downstream consumers (e.g. register allocation) can index by id.
        PerfScope2 perf_reid{GetPerfStats2().reid_block};
        block->ReIdInstr();
    }

    void RunFunction(HIRFunction* function, Optimizations enabled_opts) const {
        PerfScope2 perf_total{GetPerfStats2().pass_total};
        for (auto& entry : entries) {
            if (!entry.function_pass) continue;
            if (entry.required_opt == Optimizations::None || True(enabled_opts & entry.required_opt)) {
                PerfScope2 perf_pass{CounterFor(entry.required_opt)};
                entry.function_pass(function);
            }
        }
    }

    // Build the default optimization pipeline
    static PassPipeline BuildDefault(const UniformInfo* uniform_info = nullptr);

private:
    static PerfCounter2& CounterFor(Optimizations opt) {
        auto& s = GetPerfStats2();
        switch (opt) {
            case Optimizations::UniformElimination:
            case Optimizations::XmmFaultSink:
                return s.pass_uniform;
            case Optimizations::FlagElimination:
                return s.pass_flags;
            case Optimizations::ConstantFolding:
                return s.pass_const;
            case Optimizations::DeadCodeRemove:
                return s.pass_dce;
            default:
                // No default pipeline entry currently reaches this case.
                // Keeping a total-only bucket would hide future pass cost, so
                // make it visible as framework residual instead.
                return s.pass_total;
        }
    }

    std::vector<PassEntry> entries;
};

}  // namespace swift::runtime::ir
