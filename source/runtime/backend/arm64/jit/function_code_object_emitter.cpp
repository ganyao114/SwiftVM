#include "function_code_object_emitter.h"

#include "jit_context.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/ir/hir_builder.h"
#include "translator.h"

namespace swift::runtime::backend::arm64 {

FunctionCodeObjectEmitter::FunctionCodeObjectEmitter(std::shared_ptr<Module> module,
                                                     std::span<const Arm64PreparedUnit> prepared)
        : module(std::move(module)) {
    ASSERT(this->module && !prepared.empty());
    regions.reserve(prepared.size());
    for (const auto& region : prepared) {
        ASSERT(region.Function());
        regions.push_back({region.Function(), region.Allocation(), &region.Actions()});
    }
}

FunctionCodeObjectEmitter::~FunctionCodeObjectEmitter() = default;

bool FunctionCodeObjectEmitter::Emit(bool enable_direct_link) {
    translators.clear();
    contexts.clear();
    for (const auto& region : regions)
        if (!ARM64UnitReadyForEmission(
                    module, region.reg_alloc, *region.actions, nullptr, region.function))
            return false;
    contexts.reserve(regions.size());
    translators.reserve(regions.size());

    for (const auto& region : regions) {
        if (contexts.empty()) {
            contexts.push_back(std::make_unique<JitContext>(
                    module, region.reg_alloc, *region.actions, enable_direct_link));
        } else {
            contexts.push_back(std::make_unique<JitContext>(module,
                                                            region.reg_alloc,
                                                            *region.actions,
                                                            contexts.front()->GetMasm(),
                                                            enable_direct_link));
        }
        translators.push_back(
                std::make_unique<JitTranslator>(*contexts.back()));
    }

    // No entry may be emitted until every region in the object has passed its
    // whole-unit certificate. Context construction and preflight emit no bytes.
    for (size_t i = 0; i < regions.size(); ++i)
        if (!translators[i]->ImportUnitEdges(nullptr, regions[i].function)) return false;
    // This closed interval changes neither IR nor allocation. Consume the
    // preflight proof once; the private body cannot be called by other clients.
    for (size_t i = 0; i < regions.size(); ++i)
        if (!translators[i]->EmitPreparedFunction(regions[i].function)) return false;

    for (size_t i = 1; i < contexts.size(); ++i) {
        contexts.front()->AbsorbEmissionState(*contexts[i]);
    }
    return true;
}

u32 FunctionCodeObjectEmitter::CurrentBufferSize() const {
    return contexts.empty() ? 0 : Context().CurrentBufferSize();
}

bool FunctionCodeObjectEmitter::RequiresRegionTrampoline() const {
    return Context().RequiresRegionTrampoline();
}

JitContext& FunctionCodeObjectEmitter::Context() const {
    ASSERT(!contexts.empty());
    return *contexts.front();
}

JitTranslator& FunctionCodeObjectEmitter::Translator(size_t region) const {
    ASSERT(region < translators.size());
    return *translators[region];
}

u8* FunctionCodeObjectEmitter::Flush(const CodeBuffer& buffer) {
    return Context().Flush(buffer);
}

}  // namespace swift::runtime::backend::arm64
