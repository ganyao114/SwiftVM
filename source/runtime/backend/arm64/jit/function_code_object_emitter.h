#pragma once

#include <memory>
#include <span>
#include <vector>

#include "runtime/backend/code_cache.h"
#include "runtime/compile/allocate/allocation.h"

namespace swift::runtime::ir {
class HIRFunction;
}

namespace swift::runtime::backend {
class Module;
}

namespace swift::runtime::backend::arm64 {

class JitContext;
class JitTranslator;
class Arm64PreparedUnit;
class UnitActions;

class FunctionCodeObjectEmitter final {
public:
    FunctionCodeObjectEmitter(std::shared_ptr<Module> module,
                              std::span<const Arm64PreparedUnit> regions);
    ~FunctionCodeObjectEmitter();

    bool Emit(bool enable_direct_link = true);
    [[nodiscard]] u32 CurrentBufferSize() const;
    [[nodiscard]] bool RequiresRegionTrampoline() const;
    [[nodiscard]] JitContext& Context() const;
    [[nodiscard]] JitTranslator& Translator(size_t region) const;
    [[nodiscard]] u8* Flush(const CodeBuffer& buffer);

private:
    struct Region {
        const ir::HIRFunction* function;
        compile::AllocationView reg_alloc;
        const UnitActions* actions;
    };
    std::shared_ptr<Module> module;
    std::vector<Region> regions;
    std::vector<std::unique_ptr<JitContext>> contexts;
    std::vector<std::unique_ptr<JitTranslator>> translators;
};

}  // namespace swift::runtime::backend::arm64
