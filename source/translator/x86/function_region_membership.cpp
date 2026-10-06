#include "function_region_membership.h"

#include <algorithm>

namespace swift::translator::x86 {

namespace {
constexpr size_t kRegionBlockBudget = 64;
constexpr size_t kCodeObjectBlockBudget = 128;
}  // namespace

void FunctionRegionMembership::Record(runtime::IntrusivePtr<runtime::ir::Function> owner,
                                      Region region,
                                      std::span<const runtime::LocationDescriptor> pending_roots) {
    std::vector<runtime::LocationDescriptor> unique_pending{pending_roots.begin(),
                                                            pending_roots.end()};
    std::sort(unique_pending.begin(), unique_pending.end());
    unique_pending.erase(std::unique(unique_pending.begin(), unique_pending.end()),
                         unique_pending.end());
    std::erase(unique_pending, region.root);
    if (!owner || unique_pending.empty()) {
        return;
    }
    std::lock_guard guard(mutex);

    for (const auto root : unique_pending) {
        if (const auto it = groups.find(root); it != groups.end()) {
            Erase(it->second);
        }
    }
    auto group = std::make_shared<Group>(Group{
            .owner = std::move(owner),
            .region = region,
            .pending_roots = std::move(unique_pending),
    });
    for (const auto root : group->pending_roots) {
        groups.emplace(root, group);
    }
}

std::optional<FunctionRegionMembership::Selection> FunctionRegionMembership::Select(
        runtime::LocationDescriptor root) {
    std::lock_guard guard(mutex);
    const auto it = groups.find(root);
    if (it == groups.end()) {
        return std::nullopt;
    }
    auto group = it->second;
    Erase(group);
    if (group->region.decoded_blocks + kRegionBlockBudget > kCodeObjectBlockBudget) {
        return std::nullopt;
    }
    return Selection{
            .retired_owner = std::move(group->owner),
            .retired_region = group->region,
    };
}

void FunctionRegionMembership::Erase(const std::shared_ptr<Group>& group) {
    for (const auto root : group->pending_roots) {
        if (const auto it = groups.find(root); it != groups.end() && it->second == group) {
            groups.erase(it);
        }
    }
}

}  // namespace swift::translator::x86
