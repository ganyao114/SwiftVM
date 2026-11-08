#pragma once

#include "aarch64/constants-aarch64.h"
#include "runtime/arch/arm64/service.h"

namespace swift::arm64::detail {

inline runtime::arm64::StateServiceRequest StateRequest(std::uint32_t word) noexcept {
    using namespace vixl::aarch64;
    using Kind = runtime::arm64::StateServiceKind;
    runtime::arm64::StateServiceRequest request;
    const auto rn = std::uint8_t((word >> 5) & 31), rt = std::uint8_t(word & 31),
               rt2 = std::uint8_t((word >> 10) & 31), rs = std::uint8_t((word >> 16) & 31);
    if ((word & LoadStoreExclusiveFMask) == LoadStoreExclusiveFixed && !(word & (1u << 23))) {
        const bool load = word & (1u << 22), pair = word & (1u << 21);
        // CASP occupies the pair encodings with size 0/1. Scalar exclusives
        // reserve Rt2=31; loads reserve Rs=31. Overlap is unpredictable.
        if ((pair && !(word & (1u << 31))) || (!pair && rt2 != 31) || (load && rs != 31) ||
            (load && pair && rt == rt2) ||
            (!load && (rs == rt || (pair && rs == rt2) || (rs == rn && rn != 31))))
            return request;
        request = {load ? Kind::LoadExclusive : Kind::StoreExclusive,
                   std::uint8_t(word >> 30),
                   pair,
                   bool(word & (1u << 15)),
                   rn,
                   rt,
                   rt2,
                   rs};
    } else if ((word & SystemExclusiveMonitorMask) == CLREX) {
        request.kind = Kind::ClearExclusive;
    } else if ((word & SystemSysRegMask) == MRS) {
        const auto reg = (word >> 5) & 0xffff;
        if (reg == CNTVCT_EL0)
            request.kind = Kind::ReadVirtualCounter;
        else if (reg == CNTPCT_EL0)
            request.kind = Kind::ReadPhysicalCounter;
        else if (reg == CNTFREQ_EL0)
            request.kind = Kind::ReadCounterFrequency;
        else if (reg == CTR_EL0)
            request.kind = Kind::ReadCacheType;
        request.first_register = rt;
    } else {
        const auto operation = word & ~31u;
        if (operation == (SYS | (CVAU << SysOp_offset)))
            request.kind = Kind::DataCacheCleanPoU;
        else if (operation == (SYS | (CVAC << SysOp_offset)))
            request.kind = Kind::DataCacheCleanPoC;
        else if (operation == (SYS | (CIVAC << SysOp_offset)))
            request.kind = Kind::DataCacheCleanInvalidatePoC;
        else if (operation == (SYS | (IVAU << SysOp_offset)))
            request.kind = Kind::InstructionCacheInvalidatePoU;
        request.address_register = rt;
    }
    return request;
}
}  // namespace swift::arm64::detail
