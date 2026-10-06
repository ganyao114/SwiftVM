#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "runtime/ir/hir_builder.h"
#include "translator/x86/function_decode_frontier.h"
#include "translator/x86/function_region_decoder.h"

namespace {

using namespace swift::runtime;
using namespace swift::runtime::ir;
using swift::translator::x86::FunctionDecodeFrontier;
using swift::translator::x86::FunctionRegionDecoder;

class BoundaryMemory final : public MemoryInterface {
public:
    bool Read(void*, size_t, size_t) override {
        throw std::runtime_error("decoding crossed an external boundary");
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void*) override {
        throw std::runtime_error("instruction fetch crossed an external boundary");
    }
};

TEST_CASE("function region decode stops at unavailable external roots",
          "[function-entry][frontier]") {
    for (const bool published : {false, true}) {
        CAPTURE(published);
        constexpr LocationDescriptor kStart = 0x6000;
        constexpr LocationDescriptor kTarget = kStart + 4;
        HIRBuilder builder{8, true, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{kStart});
        builder.AdvancePC(Imm{8u});
        function->GetCurrentBlock()->GetBlock()->SetTerminal(
                terminal::ReturnToDispatch{});
        for (LocationDescriptor source : {0x6100, 0x6200, 0x6300}) {
            builder.SetCurBlock(function->CreateOrGetBlock(Location{source}));
            builder.RegisterExternalDirectLink(Location{kTarget});
            builder.ExternalLinkBlock(terminal::ExternalLinkBlock{Location{kTarget}});
        }

        BoundaryMemory memory;
        size_t boundary_checks{};
        FunctionRegionDecoder decoder{
                builder, *function,
                {.entry = kStart,
                 .block_cap = 8,
                 .lazy = true,
                 .memory = &memory,
                 .local_target = [&](LocationDescriptor address) {
                     // Fail promptly if the same ineligible root is rediscovered.
                     if (++boundary_checks > 32) {
                         throw std::runtime_error("external frontier did not converge");
                     }
                     return published || address != kTarget;
                 },
                 .has_code = [&](LocationDescriptor address) {
                     return published && address == kTarget;
                 }}};
        swift::translator::x86::FunctionRegionDecodeResult result;
        REQUIRE_NOTHROW(result = decoder.Decode());
        REQUIRE(result.decoded_count == 0);
        REQUIRE_FALSE(result.hit_block_cap);
        REQUIRE(result.pending_roots.empty());
        REQUIRE(function->GetExternalEntryRoots().empty());
        REQUIRE(function->GetExternalDirectLinks().size() == 3);
    }
}

TEST_CASE("function region decode preserves already queued published entries",
          "[function-entry][frontier]") {
    for (const bool lazy : {false, true}) {
        CAPTURE(lazy);
        constexpr LocationDescriptor kStart = 0x6800;
        constexpr LocationDescriptor kTarget = kStart + 4;
        HIRBuilder builder{8, true, false, FeatureSet{}};
        auto* function = builder.AppendFunction(Location{kStart});
        auto* owner = function->GetCurrentBlock();
        builder.AdvancePC(Imm{8u});
        owner->GetBlock()->SetTerminal(terminal::ReturnToDispatch{});
        function->CreateOrGetBlock(Location{kTarget});

        BoundaryMemory memory;
        FunctionRegionDecoder decoder{
                builder, *function,
                {.entry = kStart, .block_cap = 8, .lazy = lazy,
                 .memory = &memory,
                 .local_target = [](LocationDescriptor) { return true; },
                 .has_code = [](LocationDescriptor address) {
                     return address == kTarget;
                 }}};
        swift::translator::x86::FunctionRegionDecodeResult result;
        REQUIRE_NOTHROW(result = decoder.Decode());
        REQUIRE(result.decoded_count == 0);
        REQUIRE_FALSE(result.hit_block_cap);
        REQUIRE(result.pending_roots.empty());
        REQUIRE(FunctionDecodeFrontier::DecodedEnd(owner->GetBlock()) == kStart + 8);
        REQUIRE(owner->GetInstList().size() == 1);
        REQUIRE(function->GetFunctionEntryProvenance().empty());
    }
}

TEST_CASE("function decode frontier preserves split provenance",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x1000;
    constexpr LocationDescriptor kTarget = kStart + 4;
    constexpr LocationDescriptor kEnd = kStart + 8;

    HIRBuilder builder{4, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    auto* owner = function->GetCurrentBlock();
    builder.AdvancePC(Imm{kEnd - kStart});
    builder.AddGuestCodeDependency(Location{kStart + 1}, Location{kTarget});
    REQUIRE(function->CreateOrGetBlock(Location{kTarget}) != nullptr);

    FunctionDecodeFrontier frontier{function};
    const auto split = frontier.FindSplit(kTarget);
    REQUIRE(split);
    REQUIRE(split->owner == owner);
    REQUIRE(split->provenance != nullptr);
    REQUIRE(split->provenance->owner_start == Location{kStart});
    REQUIRE(split->provenance->owner_end == Location{kEnd});
    REQUIRE(split->provenance->dependencies ==
            std::vector<FunctionEntrySourceRange>{
                    {Location{kStart + 1}, Location{kTarget}}});

    REQUIRE(builder.ResetDecodedBlock(owner));
    builder.AdvancePC(Imm{kTarget - kStart});
    frontier.Accept(kTarget);
    REQUIRE(frontier.IsAccepted(kTarget));
    REQUIRE_FALSE(frontier.FindSplit(kTarget));

    function->SetFunctionEntryProvenance(frontier.ExportProvenance());
    const auto& provenance = function->GetFunctionEntryProvenance();
    REQUIRE(provenance.size() == 1);
    REQUIRE(provenance.front().IsAccepted());
    REQUIRE(provenance.front().dependencies ==
            std::vector<FunctionEntrySourceRange>{
                    {Location{kStart + 1}, Location{kTarget}}});
}

TEST_CASE("function decode frontier transfers call return ownership",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x2000;
    constexpr LocationDescriptor kSplit = kStart + 4;
    constexpr LocationDescriptor kEnd = kStart + 8;
    constexpr LocationDescriptor kReturn = kEnd + 4;

    HIRBuilder builder{4, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    auto* owner = function->GetCurrentBlock();
    builder.AdvancePC(Imm{kEnd - kStart});
    builder.RegisterCallReturn(Location{kReturn});
    auto* return_block = function->CreateOrGetBlock(Location{kReturn});
    REQUIRE(return_block->IsCallReturnBlock());

    FunctionDecodeFrontier frontier{function};
    const auto split = frontier.FindExternalSplit(kSplit);
    REQUIRE(split);
    REQUIRE(split->provenance->call_return_owned);
    REQUIRE_FALSE(split->provenance->call_return_boundary);
    REQUIRE(builder.ResetDecodedBlock(owner));
    REQUIRE(owner->GetCallReturnBlock() == nullptr);
    REQUIRE_FALSE(return_block->IsCallReturnBlock());

    builder.AdvancePC(Imm{kSplit - kStart});
    builder.ExternalLinkBlock(
            terminal::ExternalLinkBlock{Location{kSplit}});
    auto* root = function->CreateOrGetBlock(Location{kSplit});
    REQUIRE(owner->GetSuccessors().empty());
    REQUIRE(root->GetPredecessors().empty());
    builder.SetCurBlock(root);
    builder.AdvancePC(Imm{kEnd - kSplit});
    builder.RegisterCallReturn(Location{kReturn});
    REQUIRE(root->GetCallReturnBlock() == return_block);
    REQUIRE(return_block->IsCallReturnBlock());
    frontier.Accept(kSplit);

    constexpr LocationDescriptor kMissing = kReturn + 4;
    REQUIRE_FALSE(frontier.FindSplit(kMissing));
    const auto* missing = frontier.FindProvenance(kMissing);
    REQUIRE(missing != nullptr);
    REQUIRE(missing->rejection == FunctionEntryRejection::NoOwner);
    REQUIRE_FALSE(frontier.FindSplit(kMissing));
}

TEST_CASE("function decode frontier identifies call return boundaries",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x2800;
    constexpr LocationDescriptor kReturn = kStart + 4;
    constexpr LocationDescriptor kEnd = kStart + 8;

    HIRBuilder builder{4, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    builder.AdvancePC(Imm{kEnd - kStart});
    builder.RegisterCallReturn(Location{kReturn});

    FunctionDecodeFrontier frontier{function};
    const auto split = frontier.FindExternalSplit(kReturn);
    REQUIRE(split);
    REQUIRE(split->provenance->call_return_owned);
    REQUIRE(split->provenance->call_return_boundary);
}

TEST_CASE("function decode frontier keeps an internal predecessor for external roots",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x3000;
    constexpr LocationDescriptor kTarget = kStart + 4;
    constexpr LocationDescriptor kEnd = kStart + 8;

    HIRBuilder builder{4, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    auto* owner = function->GetCurrentBlock();
    builder.AdvancePC(Imm{kEnd - kStart});

    FunctionDecodeFrontier frontier{function};
    const auto split = frontier.FindExternalSplit(kTarget);
    REQUIRE(split);
    REQUIRE(split->provenance->external_root);
    auto* root = function->CreateOrGetBlock(Location{kTarget});
    REQUIRE(root != nullptr);

    REQUIRE(builder.ResetDecodedBlock(owner));
    builder.AdvancePC(Imm{kTarget - kStart});
    builder.LinkBlock(terminal::LinkBlock{Location{kTarget}});
    frontier.Accept(kTarget);
    function->EndFunction();
    function->ComputeRPO();

    REQUIRE(owner->GetSuccessors().size() == 1);
    REQUIRE(owner->GetSuccessors().front() == root);
    REQUIRE(root->GetPredecessors().size() == 1);
    REQUIRE(root->GetPredecessors().front() == owner);
    REQUIRE(function->GetExternalEntryRoots().size() == 1);
    REQUIRE(function->GetExternalEntryRoots().front() == root);

    std::vector<Location> entries;
    for (auto& block : function->GetHIRBlocksRPO()) {
        entries.push_back(block.GetBlock()->GetStartLocation());
    }
    REQUIRE(entries == std::vector<Location>{Location{kStart}, Location{kTarget}});
}

TEST_CASE("function decode frontier promotes shared external targets",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x4000;
    constexpr LocationDescriptor kTarget = kStart + 4;
    constexpr LocationDescriptor kEnd = kStart + 8;

    HIRBuilder builder{8, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    builder.AdvancePC(Imm{kEnd - kStart});
    for (LocationDescriptor source : {0x4100, 0x4200, 0x4300}) {
        builder.SetCurBlock(function->CreateOrGetBlock(Location{source}));
        builder.RegisterExternalDirectLink(Location{kTarget});
    }

    FunctionDecodeFrontier frontier{function};
    REQUIRE(frontier.DiscoverExternalRoots(4).empty());
    REQUIRE(frontier.FindProvenance(kTarget) == nullptr);
    REQUIRE(frontier.DiscoverExternalRoots(
                    3, FunctionDecodeFrontier::OwnerPolicy::Interior)
                    .empty());
    REQUIRE(frontier.DiscoverExternalRoots(3) ==
            std::vector<LocationDescriptor>{kTarget});
    const auto* provenance = frontier.FindProvenance(kTarget);
    REQUIRE(provenance != nullptr);
    REQUIRE(provenance->external_root);
    REQUIRE(provenance->owner_start == Location{kStart});
    REQUIRE(provenance->owner_end == Location{kEnd});
}

TEST_CASE("function decode frontier admits singleton interior targets",
          "[function-entry][frontier]") {
    constexpr LocationDescriptor kStart = 0x5000;
    constexpr LocationDescriptor kOwner = 0x5010;
    constexpr LocationDescriptor kTarget = 0x5014;
    constexpr LocationDescriptor kEnd = 0x5018;

    HIRBuilder builder{4, true, false, FeatureSet{}};
    auto* function = builder.AppendFunction(Location{kStart});
    auto* owner = builder.LinkBlock(terminal::LinkBlock{Location{kOwner}});
    builder.SetCurBlock(owner);
    builder.AdvancePC(Imm{kEnd - kOwner});
    builder.SetCurBlock(function->CreateOrGetBlock(Location{0x5100}));
    builder.RegisterExternalDirectLink(Location{kTarget});

    FunctionDecodeFrontier frontier{function};
    REQUIRE(frontier.DiscoverExternalRoots(
                    1, FunctionDecodeFrontier::OwnerPolicy::Interior) ==
            std::vector<LocationDescriptor>{kTarget});
    const auto* provenance = frontier.FindProvenance(kTarget);
    REQUIRE(provenance != nullptr);
    REQUIRE(provenance->owner_start == Location{kOwner});
}

}  // namespace
