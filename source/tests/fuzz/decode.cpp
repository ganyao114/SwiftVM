#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "runtime/frontend/arm32/decode.h"
#include "runtime/frontend/arm64/decode.h"
#include "runtime/frontend/x86/decode.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span bytes{data, size};
#if SVM_FUZZ_ISA == 1
    const auto decoded = swift::x86::DecodeOne(bytes, 0x10000000);
    if (decoded && (!decoded.instruction.View().Size() ||
                    decoded.instruction.View().Size() > size))
        std::abort();
#elif SVM_FUZZ_ISA == 2
    const auto decoded = swift::arm64::DecodeOne(bytes, 0x10000000);
    if (decoded && size < 4) std::abort();
#else
    for (bool thumb : {false, true}) {
        const auto pc = swift::runtime::arm32::Location::Make(0x10000000, thumb).Key();
        const auto decoded = swift::arm32::DecodeOne(bytes, pc);
        if (decoded && (!decoded.instruction.Length() || decoded.instruction.Length() > size))
            std::abort();
    }
#endif
    return 0;
}
