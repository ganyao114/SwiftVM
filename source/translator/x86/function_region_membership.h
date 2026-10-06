#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/ir/function.h"

namespace swift::translator::x86 {

class FunctionRegionMembership final {
public:
    struct Region {
        runtime::LocationDescriptor root{};
        size_t decoded_blocks{};

        auto operator<=>(const Region&) const = default;
    };

    struct Selection {
        runtime::IntrusivePtr<runtime::ir::Function> retired_owner;
        Region retired_region;
    };

    void Record(runtime::IntrusivePtr<runtime::ir::Function> owner,
                Region region,
                std::span<const runtime::LocationDescriptor> pending_roots);
    [[nodiscard]] std::optional<Selection> Select(runtime::LocationDescriptor root);

private:
    struct Group {
        runtime::IntrusivePtr<runtime::ir::Function> owner;
        Region region;
        std::vector<runtime::LocationDescriptor> pending_roots;
    };

    void Erase(const std::shared_ptr<Group>& group);

    std::map<runtime::LocationDescriptor, std::shared_ptr<Group>> groups;
    std::mutex mutex;
};

}  // namespace swift::translator::x86
