#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <fmt/format.h>

namespace swift::translator {

class FunctionCompileStats {
public:
    explicit FunctionCompileStats(std::string_view isa) : isa(isa) {
        const char* env = std::getenv("SVM_FUNC_STATS");
        enabled = env && std::strcmp(env, "0") != 0;
    }

    ~FunctionCompileStats() {
        if (!enabled) {
            return;
        }
        const double average =
                compiled ? static_cast<double>(compiled_blocks) / static_cast<double>(compiled)
                         : 0.0;
        fmt::print(stderr,
                   "[func-stats] isa={} attempted={} compiled={} fallback_exception={} "
                   "fallback_block_cap={} compiled_blocks={} avg_blocks={:.2f}\n",
                   isa,
                   attempted,
                   compiled,
                   fallback_exception,
                   fallback_block_cap,
                   compiled_blocks,
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
    uint64_t attempted{};
    uint64_t compiled{};
    uint64_t fallback_exception{};
    uint64_t fallback_block_cap{};
    uint64_t compiled_blocks{};
};

}  // namespace swift::translator
