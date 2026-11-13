#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "runtime/ir/program.h"

namespace swift::runtime::backend::detail {

// Per-request scratch for the actual allocation schedule. No node-sized
// storage, edge mirror or backend Home representation is introduced here.
// Block and single-exit Function callers leave this empty and skip BuildOrder.
class AllocationCFG {
public:
    AllocationCFG() = default;
    AllocationCFG(const AllocationCFG&) = delete;
    AllocationCFG& operator=(const AllocationCFG&) = delete;

    void BuildOrder(const ir::Program&, std::vector<std::uint32_t>& order);
    void CloseLoops(const ir::Program&, const std::vector<std::uint32_t>& order);

    std::uint32_t Start(std::uint32_t block) const noexcept {
        return starts.empty() ? 0u : starts[block];
    }

    // Called from the backend's existing definition-position walk, not from
    // an additional scan. Rank is the same DFS scratch reused as final RPO rank.
    void RecordBlock(std::uint32_t block,
                     std::uint32_t position,
                     const ir::StoredTerminal& terminal) noexcept {
        starts[block] = position;
        const auto edge = [&](ir::BlockId target) {
            backedge |= rank[target.value] <= rank[block];
        };
        if (terminal.kind == ir::TerminalKind::Jump) edge(terminal.first);
        if (terminal.kind == ir::TerminalKind::Branch) {
            edge(terminal.first);
            edge(terminal.second);
        }
    }

    // Cross-block use extension is inlined at the real use update. Callers
    // update their own Home directly and retain their same-block fast path.
    std::uint32_t UseEnd(std::uint32_t definition, std::uint32_t use) const noexcept {
        auto end = use;
        if (ends.empty()) return end;
        auto left = base + static_cast<std::size_t>(
                                   std::upper_bound(headers.begin(), headers.end(), definition) -
                                   headers.begin());
        auto right = base + static_cast<std::size_t>(
                                    std::upper_bound(headers.begin(), headers.end(), end) -
                                    headers.begin());
        while (left < right) {
            if (left & 1) end = std::max(end, ends[left++]);
            if (right & 1) end = std::max(end, ends[--right]);
            left /= 2;
            right /= 2;
        }
        return end;
    }

private:
    static constexpr std::size_t inline_blocks = 64;
    std::array<std::uint32_t, inline_blocks> local_rank, local_starts, local_headers;
    std::array<std::uint32_t, inline_blocks * 2> local_ends;
    std::vector<std::uint32_t> rank_storage, starts_storage, headers_storage, ends_storage;
    std::span<std::uint32_t> rank, starts, headers, ends;
    std::size_t base{1};
    bool backedge{};
};

}  // namespace swift::runtime::backend::detail
