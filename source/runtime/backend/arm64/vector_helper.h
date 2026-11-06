#pragma once

#include <cstdint>

namespace swift::runtime::backend::arm64 {
// Private, integer-only numerical helper ABI. The caller snapshots three
// Block128 arguments into its own aligned 48-byte stack area before BLR.
// x0=callback, x16=metadata, x17=argument pointer. The callback's C ABI is
// Block128(u64 metadata, const Block128*) noexcept; its result becomes v0.
// All actual homes, raw PF/AF and NZCV survive. No guest state is observed.
std::uintptr_t VectorHelperBridge() noexcept;
} // namespace swift::runtime::backend::arm64
