//
// Created by SwiftVM on 2024/7/1.
//

#pragma once

#include <functional>
#include <vector>
#include "base/common_funcs.h"
#include "base/types.h"
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
        for (auto& entry : entries) {
            if (!entry.block_pass) continue;
            if (entry.required_opt == Optimizations::None || True(enabled_opts & entry.required_opt)) {
                entry.block_pass(block);
            }
        }
        // Passes may remove instructions, leaving non-dense ids. Re-id so that
        // downstream consumers (e.g. register allocation) can index by id.
        block->ReIdInstr();
    }

    void RunFunction(HIRFunction* function, Optimizations enabled_opts) const {
        for (auto& entry : entries) {
            if (!entry.function_pass) continue;
            if (entry.required_opt == Optimizations::None || True(enabled_opts & entry.required_opt)) {
                entry.function_pass(function);
            }
        }
    }

    // Build the default optimization pipeline
    static PassPipeline BuildDefault(const UniformInfo* uniform_info = nullptr);

private:
    std::vector<PassEntry> entries;
};

}  // namespace swift::runtime::ir
