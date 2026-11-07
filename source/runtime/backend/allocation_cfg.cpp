#include "allocation_cfg.h"

namespace swift::runtime::backend::detail {
using namespace ir;
using u32 = std::uint32_t;

void AllocationCFG::BuildOrder(const Program& program, std::vector<u32>& order) {
    const auto blocks = program.Blocks();
    const auto entry = program.EntryBlock();
    rank.resize(blocks.size());
    starts.resize(blocks.size());

    struct Frame {
        u32 block, edge;
    };

    std::vector<Frame> stack;
    stack.reserve(blocks.size());
    order.reserve(blocks.size());
    const auto component = [&](u32 root) {
        const auto begin = order.size();
        rank[root] = 1;
        stack.push_back({root, 0});
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto& terminal = blocks[frame.block].terminal;
            BlockId next;
            if (terminal.kind == TerminalKind::Jump && frame.edge < 1) {
                next = terminal.first;
                ++frame.edge;
            } else if (terminal.kind == TerminalKind::Branch && frame.edge < 2) {
                next = frame.edge++ == 0 ? terminal.first : terminal.second;
            } else {
                rank[frame.block] = 2;
                order.push_back(frame.block);
                stack.pop_back();
                continue;
            }
            if (!rank[next.value]) {
                rank[next.value] = 1;
                stack.push_back({next.value, 0});
            }
        }
        std::reverse(order.begin() + begin, order.end());
    };
    component(entry.value);
    for (u32 b = 0; b < blocks.size(); ++b)
        if (!rank[b]) component(b);
    for (u32 i = 0; i < order.size(); ++i) rank[order[i]] = i;
}

void AllocationCFG::CloseLoops(const Program& program, const std::vector<u32>& order) {
    const auto blocks = program.Blocks();
    if (backedge) {
        headers.reserve(blocks.size());
        for (const auto b : order) headers.push_back(starts[b]);
        // Scheduled starts are already nondecreasing, including empty blocks.
        headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
        while (base < headers.size()) base *= 2;
        ends.resize(base * 2);
        for (const auto b : order) {
            const auto& body = blocks[b];
            const auto add = [&](BlockId target) {
                // Rank distinguishes true retreating edges even where empty
                // blocks share their scheduled node coordinate.
                if (rank[target.value] > rank[b]) return;
                const auto h = starts[target.value];
                const auto at = base + static_cast<std::size_t>(
                                               std::lower_bound(headers.begin(), headers.end(), h) -
                                               headers.begin());
                ends[at] = std::max(ends[at], starts[b] + body.node_count);
            };
            if (body.terminal.kind == TerminalKind::Jump) add(body.terminal.first);
            if (body.terminal.kind == TerminalKind::Branch) {
                add(body.terminal.first);
                add(body.terminal.second);
            }
        }
        for (auto at = base; at-- > 1;) ends[at] = std::max(ends[at * 2], ends[at * 2 + 1]);
        // Crossing/irreducible backedges need transitive interval closure:
        // entering a second header before the first source end can carry a
        // still-needed value beyond that end. Right-hand headers are already
        // closed, so one right-to-left range query per header suffices.
        for (auto i = headers.size(); i-- > 0;) {
            auto end = ends[base + i];
            if (!end) continue;
            auto left = base + i + 1;
            auto right = base + static_cast<std::size_t>(
                                        std::upper_bound(headers.begin(), headers.end(), end) -
                                        headers.begin());
            while (left < right) {
                if (left & 1) end = std::max(end, ends[left++]);
                if (right & 1) end = std::max(end, ends[--right]);
                left /= 2;
                right /= 2;
            }
            auto at = base + i;
            ends[at] = end;
            while ((at /= 2)) ends[at] = std::max(ends[at * 2], ends[at * 2 + 1]);
        }
    }
}

}  // namespace swift::runtime::backend::detail
