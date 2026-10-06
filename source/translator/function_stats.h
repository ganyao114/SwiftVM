#pragma once

#include <cstddef>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <fmt/format.h>
#include "runtime/common/svm_config.h"

namespace swift::translator {

class FunctionCompileStats {
public:
    explicit FunctionCompileStats(std::string_view isa) : isa(isa) {
        enabled = runtime::GetSvmConfig().func_stats;
    }

    ~FunctionCompileStats() {
        if (!enabled) {
            return;
        }
        const double average =
                compiled.load() ? static_cast<double>(compiled_blocks.load()) / static_cast<double>(compiled.load())
                         : 0.0;
        fmt::print(stderr,
                   "[func-stats] isa={} attempted={} compiled={} fallback_exception={} "
                   "fallback_block_cap={} compiled_blocks={} avg_blocks={:.2f}\n",
                   isa,
                   attempted.load(),
                   compiled.load(),
                   fallback_exception.load(),
                   fallback_block_cap.load(),
                   compiled_blocks.load(),
                   average);
    }

    void Attempt() {
        if (enabled) {
            ++attempted;
        }
    }

    void Compiled(size_t blocks) {
        if (enabled) {
            ++compiled;
            compiled_blocks += blocks;
        }
    }

    void Exception(uint64_t pc, const char* what) {
        if (enabled) {
            ++fallback_exception;
            fmt::print(stderr, "[func-stats] isa={} fallback=exception pc={:#x} what={}\n",
                       isa, pc, what);
        }
    }

    void BlockCap(uint64_t pc, size_t blocks) {
        if (enabled) {
            ++fallback_block_cap;
            fmt::print(stderr, "[func-stats] isa={} fallback=block-cap pc={:#x} blocks={}\n",
                       isa, pc, blocks);
        }
    }

private:
    std::string_view isa;
    bool enabled{};
    std::atomic<uint64_t> attempted{};
    std::atomic<uint64_t> compiled{};
    std::atomic<uint64_t> fallback_exception{};
    std::atomic<uint64_t> fallback_block_cap{};
    std::atomic<uint64_t> compiled_blocks{};
};

}  // namespace swift::translator
