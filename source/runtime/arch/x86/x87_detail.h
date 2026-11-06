#pragma once

#include "x87.h"
#include <bit>
#include <cstdint>

#define LITTLEENDIAN 1
#define SOFTFLOAT_FAST_INT64 1
#define FEXCORE_PRESERVE_ALL_ATTR
extern "C" {
#include <SoftFloat-3e/softfloat.h>
}
#undef FEXCORE_PRESERVE_ALL_ATTR
#undef SOFTFLOAT_FAST_INT64
#undef LITTLEENDIAN

namespace swift::runtime::x86::detail {
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
inline constexpr u16 invalid = 1, denormal = 2, stack_fault = 64;
inline constexpr u16 summary = 0x8080, c1 = 0x0200, top_mask = 0x3800;
inline constexpr u16 c0 = 0x0100, c2 = 0x0400, c3 = 0x4000, conditions = c0 | c1 | c2 | c3;
inline constexpr u64 integer_bit = u64{1} << 63;
inline constexpr extFloat80_t one{integer_bit, 0x3fff}, zero{0, 0};
inline constexpr extFloat80_t indefinite{UINT64_C(0xc000000000000000), 0xffff};

unsigned Top(const X87State&);
unsigned Physical(const X87State&, unsigned);
unsigned Tag(const X87State&, unsigned);
void SetTop(X87State&, unsigned);
void SetTag(X87State&, unsigned, unsigned);
bool Pending(X87State&);
bool Raise(X87State&, u16);
extFloat80_t Read(const X87State&, unsigned);
bool Denormal(extFloat80_t);
bool UnsupportedValue(extFloat80_t);
unsigned Classify(extFloat80_t);
void WritePhysical(X87State&, unsigned, extFloat80_t);
void Write(X87State&, unsigned, extFloat80_t);
void Pop(X87State&);
bool StackFault(X87State&, bool);
bool Missing(const X87State&, unsigned);
X87Result Push(X87State&, extFloat80_t);
softfloat_state Environment(const X87State&);
u16 Exceptions(unsigned);

bool IsNaN(extFloat80_t) noexcept;
bool IsSignaling(extFloat80_t) noexcept;
bool IsInfinity(extFloat80_t) noexcept;
bool IsZero(extFloat80_t) noexcept;
extFloat80_t CanonicalOperand(extFloat80_t) noexcept;
bool MagnitudeGreater(extFloat80_t, extFloat80_t) noexcept;
extFloat80_t FromSigned(u64, unsigned) noexcept;
extFloat80_t FromMemory(softfloat_state&, X87Format, u64, u16& extra) noexcept;
X87Result PrepareStore(X87State&, X87Command, u64);
X87Result PrepareBcd(X87State&);
void CommitBcd(X87State&);
X87Result ExecuteExtended(X87State&, u64 encoded, X87Command, unsigned, u64);
X87Result ExecuteRemainder(X87State&, bool);
X87Result ExecuteScale(X87State&);
X87Result ExecuteExtract(X87State&);
X87Result ExecuteTranscendental(X87State&, X87Transcendental);
} // namespace swift::runtime::x86::detail
