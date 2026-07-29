#pragma once

#include <cstddef>
#include <cstdint>

#include "distorm.h"

namespace swift::x86 {

// Conservative hand decoder for the high-frequency legacy integer subset.
// A false return is not an error: the caller must use distorm.
bool DecodeDistormFast(const std::uint8_t* code,
                       std::size_t available,
                       bool is_64bit,
                       _DInst& insn);

bool DistormFastEnabled();
bool DistormFastVerifyEnabled();

// Verification deliberately compares every semantically visible _DInst field,
// rather than object bytes (which may contain ABI padding).
bool DistormFastEquivalent(const _DInst& fast, const _DInst& distorm, const std::uint8_t* code);

void DistormFastRecordAttempt(bool hit);
void DistormFastRecordVerification(bool match,
                                   const _DInst& fast,
                                   const _DInst& distorm,
                                   const std::uint8_t* code);

}  // namespace swift::x86
