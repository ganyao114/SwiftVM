#pragma once

#include <map>
#include <optional>

#include "runtime/ir/hir_builder.h"

namespace swift::translator::x86 {

class FunctionDecodeFrontier final {
public:
    using Provenance = runtime::ir::FunctionEntryProvenance;
    using Rejection = runtime::ir::FunctionEntryRejection;

    enum class OwnerPolicy : u8 {
        Any,
        Interior,
    };

    struct Split {
        const runtime::ir::HIRBlock* owner{};
        runtime::LocationDescriptor target{};
        const Provenance* provenance{};
    };

    explicit FunctionDecodeFrontier(runtime::ir::HIRFunction* function)
            : function(function) {}

    [[nodiscard]] std::optional<Split> FindSplit(runtime::LocationDescriptor target);
    [[nodiscard]] std::optional<Split> FindExternalSplit(
            runtime::LocationDescriptor target);
    [[nodiscard]] std::vector<runtime::LocationDescriptor>
    DiscoverExternalRoots(
            u32 minimum_sources,
            OwnerPolicy owner_policy = OwnerPolicy::Any);
    [[nodiscard]] bool IsAccepted(runtime::LocationDescriptor target) const;
    void Accept(runtime::LocationDescriptor target);
    void Reject(runtime::LocationDescriptor target, Rejection reason);
    [[nodiscard]] const Provenance* FindProvenance(runtime::LocationDescriptor target) const;
    [[nodiscard]] std::vector<Provenance> ExportProvenance() const;

    [[nodiscard]] static runtime::LocationDescriptor DecodedEnd(
            const runtime::ir::Block* block);

private:
    [[nodiscard]] std::optional<Split> FindSplit(
            runtime::LocationDescriptor target,
            bool external_root,
            bool record_missing_owner);

    struct Record {
        Provenance provenance{};
        const runtime::ir::HIRBlock* owner{};
    };

    runtime::ir::HIRFunction* function{};
    std::map<runtime::LocationDescriptor, Record> records{};
};

}  // namespace swift::translator::x86
