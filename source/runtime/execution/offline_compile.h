#pragma once

#include "code_image.h"
#include "runtime.h"

namespace swift::runtime::execution {

// Uses the ordinary VM frontend, optimizer, allocator and publisher. Symbols
// provide entry candidates; direct exits and decoded continuations feed sweep.
CodeImage CompileGuestImage(std::span<const std::uint8_t> guest_elf,
                            const std::string& display_path,
                            std::span<const std::uint64_t> entries,
                            bool sweep,
                            RuntimeOptions,
                            RuntimeStats&);

}  // namespace swift::runtime::execution
