// Default-off execution probe for W90's AFP FPCR boundary tax.
#pragma once

#include <cstddef>
#include <span>

#include "runtime/common/types.h"

namespace swift::runtime {

enum class FpcrTaxCounter : u32 {
    RuntimeEntry,
    RuntimeReturn,
    DispatcherEntry,
    AsmInterpreter,
    CallHost,
    DirectHelper,
    MemoryCopy,
    StoreMxcsr,
    CacheLookup,
    CacheHit,
    RebuildExecuted,
    Count,
};

constexpr size_t kFpcrTaxBoundaryCount =
        static_cast<size_t>(FpcrTaxCounter::StoreMxcsr) + 1;
constexpr size_t kFpcrTaxCounterCount =
        static_cast<size_t>(FpcrTaxCounter::Count);

[[nodiscard]] bool FpcrTaxProfEnabled();
void FpcrTaxSubmit(std::span<const u64> counters);

}  // namespace swift::runtime
