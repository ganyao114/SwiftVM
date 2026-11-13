#include "allocation_cfg.h"

namespace swift::runtime::backend::detail {
using namespace ir;
using u32 = std::uint32_t;

void AllocationCFG::BuildOrder(const Program& program, std::vector<u32>& order) {
    const auto blocks = program.Blocks();
    const auto entry = program.EntryBlock();
    if (blocks.size() <= inline_blocks) {
        std::fill_n(local_rank.data(), blocks.size(), 0u);
        std::fill_n(local_starts.data(), blocks.size(), 0u);
        rank = {local_rank.data(), blocks.size()};
        starts = {local_starts.data(), blocks.size()};
    } else {
        rank_storage.assign(blocks.size(), 0u);
        starts_storage.assign(blocks.size(), 0u);
        rank = rank_storage;
        starts = starts_storage;
    }

    struct Frame {
        u32 block, edge;
    };

    std::array<Frame, inline_blocks> local_stack;
    std::size_t stack_size{};
    std::vector<Frame> heap_stack;
    const bool use_local_stack = blocks.size() <= inline_blocks;
    if (!use_local_stack) heap_stack.reserve(blocks.size());
    const auto push_frame = [&](Frame frame) {
        if (use_local_stack)
            local_stack[stack_size++] = frame;
        else
            heap_stack.push_back(frame);
    };
    const auto pop_frame = [&] {
        if (use_local_stack)
            --stack_size;
        else
            heap_stack.pop_back();
    };
    const auto top_frame = [&]() -> Frame& {
        return use_local_stack ? local_stack[stack_size - 1] : heap_stack.back();
    };
    const auto has_frames = [&]() noexcept {
        return use_local_stack ? stack_size != 0 : !heap_stack.empty();
    };

    order.reserve(blocks.size());
    const auto component = [&](u32 root) {
        const auto begin = order.size();
        rank[root] = 1;
        push_frame({root, 0});
        while (has_frames()) {
            auto& frame = top_frame();
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
                pop_frame();
                continue;
            }
            if (!rank[next.value]) {
                rank[next.value] = 1;
                push_frame({next.value, 0});
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
        if (order.size() <= inline_blocks) {
            std::size_t count{};
            for (const auto b : order) local_headers[count++] = starts[b];
            const auto unique_end =
                    std::unique(local_headers.data(), local_headers.data() + count);
            headers = {local_headers.data(),
                       static_cast<std::size_t>(unique_end - local_headers.data())};
        } else {
            headers_storage.clear();
            headers_storage.reserve(order.size());
            for (const auto b : order) headers_storage.push_back(starts[b]);
            // Scheduled starts are already nondecreasing, including empty blocks.
            headers_storage.erase(std::unique(headers_storage.begin(), headers_storage.end()),
                                  headers_storage.end());
            headers = headers_storage;
        }
        while (base < headers.size()) base *= 2;
        if (base * 2 <= local_ends.size()) {
            std::fill_n(local_ends.data(), base * 2, 0u);
            ends = {local_ends.data(), base * 2};
        } else {
            ends_storage.assign(base * 2, 0u);
            ends = ends_storage;
        }
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
