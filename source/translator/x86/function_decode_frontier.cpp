#include "function_decode_frontier.h"

namespace swift::translator::x86 {

std::optional<FunctionDecodeFrontier::Split> FunctionDecodeFrontier::FindSplit(
        runtime::LocationDescriptor target) {
    return FindSplit(target, false, true);
}

std::optional<FunctionDecodeFrontier::Split>
FunctionDecodeFrontier::FindExternalSplit(
        runtime::LocationDescriptor target) {
    return FindSplit(target, true, false);
}

std::vector<runtime::LocationDescriptor>
FunctionDecodeFrontier::DiscoverExternalRoots(
        u32 minimum_sources,
        OwnerPolicy owner_policy) {
    ASSERT(minimum_sources > 0);
    std::map<runtime::LocationDescriptor, u32> sources;
    if (function) {
        for (const auto& link : function->GetExternalDirectLinks()) {
            ++sources[link.target.Value()];
        }
    }
    std::vector<runtime::LocationDescriptor> roots;
    for (const auto& [target, count] : sources) {
        if (count < minimum_sources) {
            continue;
        }
        const auto split = FindExternalSplit(target);
        if (!split) {
            continue;
        }
        if (owner_policy == OwnerPolicy::Interior &&
            split->provenance->owner_start ==
                    function->GetFunction()->GetStartLocation()) {
            continue;
        }
        roots.push_back(target);
    }
    return roots;
}

std::optional<FunctionDecodeFrontier::Split> FunctionDecodeFrontier::FindSplit(
        runtime::LocationDescriptor target,
        bool external_root,
        bool record_missing_owner) {
    if (const auto it = records.find(target); it != records.end()) {
        if (it->second.provenance.disposition != runtime::ir::FunctionEntryDisposition::Candidate) {
            return std::nullopt;
        }
        it->second.provenance.external_root |= external_root;
        return Split{it->second.owner, target, &it->second.provenance};
    }

    Record record{};
    record.provenance.target = runtime::ir::Location{target};
    record.provenance.external_root = external_root;
    if (!function) {
        if (record_missing_owner) {
            record.provenance.disposition = runtime::ir::FunctionEntryDisposition::Rejected;
            record.provenance.rejection = Rejection::NoOwner;
            records.emplace(target, std::move(record));
        }
        return std::nullopt;
    }

    const runtime::ir::HIRBlock* owner{};
    const auto capture_owner = [&](const runtime::ir::HIRBlock* candidate) {
        auto* block = candidate->GetBlock();
        record.owner = candidate;
        record.provenance.owner_start = block->GetStartLocation();
        record.provenance.owner_end = runtime::ir::Location{DecodedEnd(block)};
        auto* call_return = candidate->GetCallReturnBlock();
        record.provenance.call_return_owned = call_return != nullptr;
        record.provenance.call_return_boundary =
                call_return &&
                call_return->GetBlock()->GetStartLocation().Value() == target;
        record.provenance.dependencies.reserve(
                block->GetGuestCodeDependencies().size());
        for (const auto& dependency : block->GetGuestCodeDependencies()) {
            record.provenance.dependencies.push_back(
                    {dependency.start, dependency.end});
        }
    };
    for (auto& hir_block : function->GetHIRBlockList()) {
        auto* block = hir_block.GetBlock();
        if (!block || block->GetInstList().empty()) {
            continue;
        }
        const auto start = block->GetStartLocation().Value();
        const auto end = DecodedEnd(block);
        if (target <= start || target >= end) {
            continue;
        }
        if (owner) {
            capture_owner(owner);
            record.provenance.disposition = runtime::ir::FunctionEntryDisposition::Rejected;
            record.provenance.rejection = Rejection::AmbiguousOwner;
            records.emplace(target, std::move(record));
            return std::nullopt;
        }
        owner = &hir_block;
    }
    if (!owner) {
        if (record_missing_owner) {
            record.provenance.disposition = runtime::ir::FunctionEntryDisposition::Rejected;
            record.provenance.rejection = Rejection::NoOwner;
            records.emplace(target, std::move(record));
        }
        return std::nullopt;
    }

    capture_owner(owner);

    auto [it, inserted] = records.emplace(target, std::move(record));
    (void)inserted;
    return Split{owner, target, &it->second.provenance};
}

bool FunctionDecodeFrontier::IsAccepted(
        runtime::LocationDescriptor target) const {
    const auto* provenance = FindProvenance(target);
    return provenance && provenance->IsAccepted();
}

void FunctionDecodeFrontier::Accept(runtime::LocationDescriptor target) {
    auto it = records.find(target);
    ASSERT(it != records.end());
    it->second.provenance.disposition = runtime::ir::FunctionEntryDisposition::Accepted;
    it->second.provenance.rejection = Rejection::None;
    if (it->second.provenance.external_root) {
        auto* root = function->CreateOrGetBlock(runtime::ir::Location{target});
        ASSERT(root);
        function->RegisterExternalEntryRoot(root);
    }
}

void FunctionDecodeFrontier::Reject(runtime::LocationDescriptor target, Rejection reason) {
    auto it = records.find(target);
    ASSERT(it != records.end());
    ASSERT(reason != Rejection::None);
    it->second.provenance.disposition = runtime::ir::FunctionEntryDisposition::Rejected;
    it->second.provenance.rejection = reason;
}

const FunctionDecodeFrontier::Provenance* FunctionDecodeFrontier::FindProvenance(
        runtime::LocationDescriptor target) const {
    const auto it = records.find(target);
    return it == records.end() ? nullptr : &it->second.provenance;
}

std::vector<FunctionDecodeFrontier::Provenance> FunctionDecodeFrontier::ExportProvenance() const {
    std::vector<Provenance> provenance;
    provenance.reserve(records.size());
    for (const auto& [target, record] : records) {
        (void)target;
        provenance.push_back(record.provenance);
    }
    return provenance;
}

runtime::LocationDescriptor FunctionDecodeFrontier::DecodedEnd(
        const runtime::ir::Block* block) {
    auto end = block->GetStartLocation().Value();
    for (const auto& inst : block->GetInstList()) {
        if (inst.GetOp() == runtime::ir::OpCode::AdvancePC) {
            end += inst.GetArg<runtime::ir::Imm>(0).Get();
        }
    }
    return end;
}

}  // namespace swift::translator::x86
