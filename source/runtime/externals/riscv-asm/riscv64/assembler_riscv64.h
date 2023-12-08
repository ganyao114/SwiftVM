/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <deque>
#include <list>

namespace art {
namespace riscv64 {

static constexpr size_t KB = 1024;
static constexpr size_t MB = KB * KB;
static constexpr size_t GB = KB * KB * KB;

template <typename T>
using ArenaDeque = std::deque<T>;

template <typename T>
using ArenaVector = std::vector<T>;

template <int n, typename T> constexpr bool IsAligned(T x) {
    static_assert((n & (n - 1)) == 0, "n is not a power of two");
    return (x & (n - 1)) == 0;
}

template <int n, typename T> inline bool IsAligned(T* x) {
    return IsAligned<n>(reinterpret_cast<const uintptr_t>(x));
}

template <typename T> struct Identity {
    using type = T;
};

#define FALLTHROUGH_INTENDED \
  do {                       \
  } while (0)

#define ALWAYS_INLINE __attribute__((always_inline))
#define CHECK(x) assert(x)
#define DCHECK(x) assert(x)

#define CHECK_ALIGNED(value, alignment)                                                            \
    CHECK(::art::IsAligned<alignment>(value)) << reinterpret_cast<const void*>(value)
#define DCHECK_ALIGNED(value, alignment) DCHECK(IsAligned<alignment>(value))

#define CHECK_OP(LHS, RHS, OP) assert((LHS) OP (RHS))
#define CHECK_EQ(x, y) CHECK_OP(x, y, ==)
#define CHECK_NE(x, y) CHECK_OP(x, y, !=)
#define CHECK_LE(x, y) CHECK_OP(x, y, <=)
#define CHECK_LT(x, y) CHECK_OP(x, y, <)
#define CHECK_GE(x, y) CHECK_OP(x, y, >=)
#define CHECK_GT(x, y) CHECK_OP(x, y, >)

#define DCHECK_EQ(x, y) CHECK_OP(x, y, ==)
#define DCHECK_NE(x, y) CHECK_OP(x, y, !=)
#define DCHECK_LE(x, y) CHECK_OP(x, y, <=)
#define DCHECK_LT(x, y) CHECK_OP(x, y, <)
#define DCHECK_GE(x, y) CHECK_OP(x, y, >=)
#define DCHECK_GT(x, y) CHECK_OP(x, y, >)

#define DISALLOW_COPY_AND_ASSIGN(TypeName)                                                         \
    TypeName(const TypeName&) = delete;                                                            \
    void operator=(const TypeName&) = delete

// Return the number of 1-bits in `x`.
template <typename T> constexpr int POPCOUNT(T x) {
    return (sizeof(T) == sizeof(uint32_t)) ? __builtin_popcount(x) : __builtin_popcountll(x);
}

// A version of static_cast that DCHECKs that the value can be precisely represented
// when converting to Dest.
template <typename Dest, typename Source> constexpr Dest dchecked_integral_cast(Source source) {
    DCHECK((static_cast<intmax_t>(std::numeric_limits<Dest>::min()) <=
                    static_cast<intmax_t>(std::numeric_limits<Source>::min()) ||
            source >= static_cast<Source>(std::numeric_limits<Dest>::min())) &&
           (static_cast<uintmax_t>(std::numeric_limits<Dest>::max()) >=
                    static_cast<uintmax_t>(std::numeric_limits<Source>::max()) ||
            source <= static_cast<Source>(std::numeric_limits<Dest>::max())));

    return static_cast<Dest>(source);
}

// A version of dchecked_integral_cast casting between an integral type and an enum type.
// When casting to an enum type, the cast does not check if the value corresponds to an enumerator.
// When casting from an enum type, the target type can be omitted and the enum's underlying type
// shall be used.

template <typename Dest, typename Source>
constexpr std::enable_if_t<!std::is_enum_v<Source>, Dest> enum_cast(Source value) {
    return static_cast<Dest>(dchecked_integral_cast<std::underlying_type_t<Dest>>(value));
}

template <typename Dest = void, typename Source> constexpr
        typename std::enable_if_t<std::is_enum_v<Source>,
                                  std::conditional_t<std::is_same_v<Dest, void>,
                                                     std::underlying_type<Source>,
                                                     Identity<Dest>>>::type
        enum_cast(Source value) {
    using return_type = typename std::conditional_t<std::is_same_v<Dest, void>,
                                                    std::underlying_type<Source>,
                                                    Identity<Dest>>::type;
    return dchecked_integral_cast<return_type>(static_cast<std::underlying_type_t<Source>>(value));
}

// When you upcast (that is, cast a pointer from type Foo to type
// SuperclassOfFoo), it's fine to use implicit_cast<>, since upcasts
// always succeed.  When you downcast (that is, cast a pointer from
// type Foo to type SubclassOfFoo), static_cast<> isn't safe, because
// how do you know the pointer is really of type SubclassOfFoo?  It
// could be a bare Foo, or of type DifferentSubclassOfFoo.  Thus,
// when you downcast, you should use this macro.

template<typename To, typename From>     // use like this: down_cast<T*>(foo);
inline To down_cast(From* f) {                   // so we only accept pointers
    static_assert(std::is_base_of_v<From, std::remove_pointer_t<To>>,
                  "down_cast unsafe as To is not a subtype of From");

    return static_cast<To>(f);
}

template<typename To, typename From>     // use like this: down_cast<T&>(foo);
inline To down_cast(From& f) {           // so we only accept references
    static_assert(std::is_base_of_v<From, std::remove_reference_t<To>>,
                  "down_cast unsafe as To is not a subtype of From");

    return static_cast<To>(f);
}

// Like sizeof, but count how many bits a type takes. Pass type explicitly.
template <typename T> constexpr size_t BitSizeOf() {
    static_assert(std::is_integral_v<T>, "T must be integral");
    using unsigned_type = std::make_unsigned_t<T>;
    static_assert(sizeof(T) == sizeof(unsigned_type), "Unexpected type size mismatch!");
    static_assert(std::numeric_limits<unsigned_type>::radix == 2, "Unexpected radix!");
    return std::numeric_limits<unsigned_type>::digits;
}

// Like sizeof, but count how many bits a type takes. Infers type from parameter.
template <typename T> constexpr size_t BitSizeOf(T /*x*/) { return BitSizeOf<T>(); }

template <typename T> constexpr T GetIntLimit(size_t bits) {
    DCHECK_NE(bits, 0u);
    DCHECK_LT(bits, BitSizeOf<T>());
    return static_cast<T>(1) << (bits - 1);
}

template <size_t kBits, typename T> constexpr bool IsInt(T value) {
    static_assert(kBits > 0, "kBits cannot be zero.");
    static_assert(kBits <= BitSizeOf<T>(), "kBits must be <= max.");
    static_assert(std::is_signed_v<T>, "Needs a signed type.");
    // Corner case for "use all bits." Can't use the limits, as they would overflow, but it is
    // trivially true.
    return (kBits == BitSizeOf<T>())
                   ? true
                   : (-GetIntLimit<T>(kBits) <= value) && (value < GetIntLimit<T>(kBits));
}

// Generate maximum/minimum values for signed/unsigned n-bit integers
template <typename T>
constexpr T MaxInt(size_t bits) {
    DCHECK(std::is_unsigned_v<T> || bits > 0u);
    DCHECK_LE(bits, BitSizeOf<T>());
    using unsigned_type = std::make_unsigned_t<T>;
    return bits == BitSizeOf<T>()
                   ? std::numeric_limits<T>::max()
                   : std::is_signed_v<T>
                             ? ((bits == 1u) ? 0 : static_cast<T>(MaxInt<unsigned_type>(bits - 1)))
                             : static_cast<T>(UINT64_C(1) << bits) - static_cast<T>(1);
}

template <typename T>
constexpr T MinInt(size_t bits) {
    DCHECK(std::is_unsigned_v<T> || bits > 0);
    DCHECK_LE(bits, BitSizeOf<T>());
    return bits == BitSizeOf<T>()
                   ? std::numeric_limits<T>::min()
                   : std::is_signed_v<T>
                             ? ((bits == 1u) ? -1 : static_cast<T>(-1) - MaxInt<T>(bits))
                             : static_cast<T>(0);
}

// Check whether an N-bit two's-complement representation can hold value.
template <typename T>
inline bool IsInt(size_t N, T value) {
    if (N == BitSizeOf<T>()) {
        return true;
    } else {
        CHECK_LT(0u, N);
        CHECK_LT(N, BitSizeOf<T>());
        T limit = static_cast<T>(1) << (N - 1u);
        return (-limit <= value) && (value < limit);
    }
}

template <size_t kBits, typename T> constexpr bool IsUint(T value) {
    static_assert(kBits > 0, "kBits cannot be zero.");
    static_assert(kBits <= BitSizeOf<T>(), "kBits must be <= max.");
    static_assert(std::is_integral_v<T>, "Needs an integral type.");
    // Corner case for "use all bits." Can't use the limits, as they would overflow, but it is
    // trivially true.
    // NOTE: To avoid triggering assertion in GetIntLimit(kBits+1) if kBits+1==BitSizeOf<T>(),
    // use GetIntLimit(kBits)*2u. The unsigned arithmetic works well for us if it overflows.
    using unsigned_type = std::make_unsigned_t<T>;
    return (0 <= value) &&
           (kBits == BitSizeOf<T>() ||
            (static_cast<unsigned_type>(value) <= GetIntLimit<unsigned_type>(kBits) * 2u - 1u));
}

template <typename T> constexpr int CLZ(T x) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    static_assert(std::is_unsigned_v<T>, "T must be unsigned");
    static_assert(std::numeric_limits<T>::radix == 2, "Unexpected radix!");
    static_assert(sizeof(T) == sizeof(uint64_t) || sizeof(T) <= sizeof(uint32_t),
                  "Unsupported sizeof(T)");
    DCHECK_NE(x, 0u);
    constexpr bool is_64_bit = (sizeof(T) == sizeof(uint64_t));
    constexpr size_t adjustment =
            is_64_bit ? 0u : std::numeric_limits<uint32_t>::digits - std::numeric_limits<T>::digits;
    return is_64_bit ? __builtin_clzll(x) : __builtin_clz(x) - adjustment;
}

template<typename T>
constexpr int CTZ(T x) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    // It is not unreasonable to ask for trailing zeros in a negative number. As such, do not check
    // that T is an unsigned type.
    static_assert(sizeof(T) == sizeof(uint64_t) || sizeof(T) <= sizeof(uint32_t),
                  "Unsupported sizeof(T)");
    DCHECK_NE(x, static_cast<T>(0));
    return (sizeof(T) == sizeof(uint64_t)) ? __builtin_ctzll(x) : __builtin_ctz(x);
}

template<typename T>
constexpr bool IsPowerOfTwo(T x) {
    static_assert(std::is_integral_v<T>, "T must be integral");
    // TODO: assert unsigned. There is currently many uses with signed values.
    return (x & (x - 1)) == 0;
}

// For rounding integers.
// Note: Omit the `n` from T type deduction, deduce only from the `x` argument.
template<typename T>
constexpr T RoundDown(T x, typename Identity<T>::type n);

template<typename T>
constexpr T RoundDown(T x, typename Identity<T>::type n) {
    DCHECK(IsPowerOfTwo(n));
    return (x & -n);
}

template<typename T>
constexpr T RoundUp(T x, std::remove_reference_t<T> n);

template<typename T>
constexpr T RoundUp(T x, std::remove_reference_t<T> n) {
    return RoundDown(x + n - 1, n);
}

// The alignment guaranteed for individual allocations.
static constexpr size_t kAlignment = 8u;

class ArenaAllocator {
public:

    void* Alloc(size_t bytes);

    void* Realloc(void *ptr, size_t old_size, size_t new_size);

    template <typename T>
    T* AllocArray(size_t length) {
        return static_cast<T*>(Alloc(length * sizeof(T)));
    }

private:
    std::list<std::vector<uint8_t>> buffer{};
};

enum XRegister {
    Zero = 0,  // X0, hard-wired zero
    RA = 1,    // X1, return address
    SP = 2,    // X2, stack pointer
    GP = 3,    // X3, global pointer (unavailable, used for shadow stack by the compiler / libc)
    TP = 4,    // X4, thread pointer (points to TLS area, not ART-internal thread)

    T0 = 5,  // X5, temporary 0
    T1 = 6,  // X6, temporary 1
    T2 = 7,  // X7, temporary 2

    S0 = 8,  // X8/FP, callee-saved 0 / frame pointer
    S1 = 9,  // X9, callee-saved 1 / ART thread register

    A0 = 10,  // X10, argument 0 / return value 0
    A1 = 11,  // X11, argument 1 / return value 1
    A2 = 12,  // X12, argument 2
    A3 = 13,  // X13, argument 3
    A4 = 14,  // X14, argument 4
    A5 = 15,  // X15, argument 5
    A6 = 16,  // X16, argument 6
    A7 = 17,  // X17, argument 7

    S2 = 18,   // X18, callee-saved 2
    S3 = 19,   // X19, callee-saved 3
    S4 = 20,   // X20, callee-saved 4
    S5 = 21,   // X21, callee-saved 5
    S6 = 22,   // X22, callee-saved 6
    S7 = 23,   // X23, callee-saved 7
    S8 = 24,   // X24, callee-saved 8
    S9 = 25,   // X25, callee-saved 9
    S10 = 26,  // X26, callee-saved 10
    S11 = 27,  // X27, callee-saved 11

    T3 = 28,  // X28, temporary 3
    T4 = 29,  // X29, temporary 4
    T5 = 30,  // X30, temporary 5
    T6 = 31,  // X31, temporary 6

    kNumberOfXRegisters = 32,
    kNoXRegister = -1,  // Signals an illegal X register.

    // Aliases.
    TR = S1,    // ART Thread Register - managed runtime
    TMP = T6,   // Reserved for special uses, such as assembler macro instructions.
    TMP2 = T5,  // Reserved for special uses, such as assembler macro instructions.
};

std::ostream& operator<<(std::ostream& os, const XRegister& rhs);

enum FRegister {
    FT0 = 0,  // F0, temporary 0
    FT1 = 1,  // F1, temporary 1
    FT2 = 2,  // F2, temporary 2
    FT3 = 3,  // F3, temporary 3
    FT4 = 4,  // F4, temporary 4
    FT5 = 5,  // F5, temporary 5
    FT6 = 6,  // F6, temporary 6
    FT7 = 7,  // F7, temporary 7

    FS0 = 8,  // F8, callee-saved 0
    FS1 = 9,  // F9, callee-saved 1

    FA0 = 10,  // F10, argument 0 / return value 0
    FA1 = 11,  // F11, argument 1 / return value 1
    FA2 = 12,  // F12, argument 2
    FA3 = 13,  // F13, argument 3
    FA4 = 14,  // F14, argument 4
    FA5 = 15,  // F15, argument 5
    FA6 = 16,  // F16, argument 6
    FA7 = 17,  // F17, argument 7

    FS2 = 18,   // F18, callee-saved 2
    FS3 = 19,   // F19, callee-saved 3
    FS4 = 20,   // F20, callee-saved 4
    FS5 = 21,   // F21, callee-saved 5
    FS6 = 22,   // F22, callee-saved 6
    FS7 = 23,   // F23, callee-saved 7
    FS8 = 24,   // F24, callee-saved 8
    FS9 = 25,   // F25, callee-saved 9
    FS10 = 26,  // F26, callee-saved 10
    FS11 = 27,  // F27, callee-saved 11

    FT8 = 28,   // F28, temporary 8
    FT9 = 29,   // F29, temporary 9
    FT10 = 30,  // F30, temporary 10
    FT11 = 31,  // F31, temporary 11

    kNumberOfFRegisters = 32,
    kNoFRegister = -1,  // Signals an illegal F register.

    FTMP = FT11,  // Reserved for special uses, such as assembler macro instructions.
};

std::ostream& operator<<(std::ostream& os, const FRegister& rhs);

class ScratchRegisterScope;

static constexpr size_t kRiscv64HalfwordSize = 2;
static constexpr size_t kRiscv64WordSize = 4;
static constexpr size_t kRiscv64DoublewordSize = 8;
static constexpr size_t kRiscv64FloatRegSizeInBytes = 8;

enum class PointerSize : size_t {
    k32 = 4,
    k64 = 8
};

static constexpr PointerSize kRiscv64PointerSize = PointerSize::k64;

static constexpr PointerSize kRuntimePointerSize = sizeof(void*) == 8U
                                                           ? PointerSize::k64
                                                           : PointerSize::k32;

// Runtime sizes.
static constexpr size_t kBitsPerByte = 8;
static constexpr size_t kBitsPerByteLog2 = 3;
static constexpr int kBitsPerIntPtrT = sizeof(intptr_t) * kBitsPerByte;

// Required stack alignment
static constexpr size_t kStackAlignment = 16;

enum class FPRoundingMode : uint32_t {
    kRNE = 0x0,  // Round to Nearest, ties to Even
    kRTZ = 0x1,  // Round towards Zero
    kRDN = 0x2,  // Round Down (towards −Infinity)
    kRUP = 0x3,  // Round Up (towards +Infinity)
    kRMM = 0x4,  // Round to Nearest, ties to Max Magnitude
    kDYN = 0x7,  // Dynamic rounding mode
    kDefault = kDYN,
    // Some instructions never need to round even though the spec includes the RM field.
    // To simplify testing, emit the RM as 0 by default for these instructions because that's what
    // `clang` does and because the `llvm-objdump` fails to disassemble the other rounding modes.
    kIgnored = 0
};

enum class AqRl : uint32_t {
    kNone = 0x0,
    kRelease = 0x1,
    kAcquire = 0x2,
    kAqRl = kRelease | kAcquire
};

// the type for fence
enum FenceType {
    kFenceNone = 0,
    kFenceWrite = 1,
    kFenceRead = 2,
    kFenceOutput = 4,
    kFenceInput = 8,
    kFenceDefault = 0xf,
};

// Used to test the values returned by FClassS/FClassD.
enum FPClassMaskType {
    kNegativeInfinity = 0x001,
    kNegativeNormal = 0x002,
    kNegativeSubnormal = 0x004,
    kNegativeZero = 0x008,
    kPositiveZero = 0x010,
    kPositiveSubnormal = 0x020,
    kPositiveNormal = 0x040,
    kPositiveInfinity = 0x080,
    kSignalingNaN = 0x100,
    kQuietNaN = 0x200,
};

class Assembler;
class AssemblerBuffer;

class MemoryRegion final {
public:
    struct ContentEquals {
        constexpr bool operator()(const MemoryRegion& lhs, const MemoryRegion& rhs) const {
            return lhs.size() == rhs.size() && memcmp(lhs.begin(), rhs.begin(), lhs.size()) == 0;
        }
    };

    MemoryRegion() : pointer_(nullptr), size_(0) {}
    MemoryRegion(void* pointer_in, uintptr_t size_in) : pointer_(pointer_in), size_(size_in) {}

    void* pointer() const { return pointer_; }
    size_t size() const { return size_; }
    size_t size_in_bits() const { return size_ * kBitsPerByte; }

    static size_t pointer_offset() {
        return offsetof(MemoryRegion, pointer_);
    }

    uint8_t* begin() const { return reinterpret_cast<uint8_t*>(pointer_); }
    uint8_t* end() const { return begin() + size_; }

    // Load value of type `T` at `offset`.  The memory address corresponding
    // to `offset` should be word-aligned (on ARM, this is a requirement).
    template<typename T>
    ALWAYS_INLINE T Load(uintptr_t offset) const {
        T* address = ComputeInternalPointer<T>(offset);
        DCHECK(IsWordAligned(address));
        return *address;
    }

    // Store `value` (of type `T`) at `offset`.  The memory address
    // corresponding to `offset` should be word-aligned (on ARM, this is
    // a requirement).
    template<typename T>
    ALWAYS_INLINE void Store(uintptr_t offset, T value) const {
        T* address = ComputeInternalPointer<T>(offset);
        DCHECK(IsWordAligned(address));
        *address = value;
    }

    // Load value of type `T` at `offset`.  The memory address corresponding
    // to `offset` does not need to be word-aligned.
    template<typename T>
    ALWAYS_INLINE T LoadUnaligned(uintptr_t offset) const {
        // Equivalent unsigned integer type corresponding to T.
        using U = std::make_unsigned_t<T>;
        U equivalent_unsigned_integer_value = 0;
        // Read the value byte by byte in a little-endian fashion.
        for (size_t i = 0; i < sizeof(U); ++i) {
            equivalent_unsigned_integer_value +=
                    *ComputeInternalPointer<uint8_t>(offset + i) << (i * kBitsPerByte);
        }
        return bit_cast<T, U>(equivalent_unsigned_integer_value);
    }

    // Store `value` (of type `T`) at `offset`.  The memory address
    // corresponding to `offset` does not need to be word-aligned.
    template<typename T>
    ALWAYS_INLINE void StoreUnaligned(uintptr_t offset, T value) const {
        // Equivalent unsigned integer type corresponding to T.
        using U = std::make_unsigned_t<T>;
        U equivalent_unsigned_integer_value = bit_cast<U, T>(value);
        // Write the value byte by byte in a little-endian fashion.
        for (size_t i = 0; i < sizeof(U); ++i) {
            *ComputeInternalPointer<uint8_t>(offset + i) =
                    (equivalent_unsigned_integer_value >> (i * kBitsPerByte)) & 0xFF;
        }
    }

    template<typename T>
    ALWAYS_INLINE T* PointerTo(uintptr_t offset) const {
        return ComputeInternalPointer<T>(offset);
    }

    void CopyFrom(size_t offset, const MemoryRegion& from) const;

    template<class Vector>
    void CopyFromVector(size_t offset, Vector& vector) const {
        if (!vector.empty()) {
            CopyFrom(offset, MemoryRegion(vector.data(), vector.size()));
        }
    }

    // Compute a sub memory region based on an existing one.
    ALWAYS_INLINE MemoryRegion Subregion(uintptr_t offset, uintptr_t size_in) const {
        CHECK_GE(this->size(), size_in);
        CHECK_LE(offset,  this->size() - size_in);
        return MemoryRegion(reinterpret_cast<void*>(begin() + offset), size_in);
    }

    // Compute an extended memory region based on an existing one.
    ALWAYS_INLINE void Extend(const MemoryRegion& region, uintptr_t extra) {
        pointer_ = region.pointer();
        size_ = (region.size() + extra);
    }

private:
    template<typename T>
    ALWAYS_INLINE T* ComputeInternalPointer(size_t offset) const {
        CHECK_GE(size(), sizeof(T));
        CHECK_LE(offset, size() - sizeof(T));
        return reinterpret_cast<T*>(begin() + offset);
    }

    // Locate the bit with the given offset. Returns a pointer to the byte
    // containing the bit, and sets bit_mask to the bit within that byte.
    ALWAYS_INLINE uint8_t* ComputeBitPointer(uintptr_t bit_offset, uint8_t* bit_mask) const {
        uintptr_t bit_remainder = (bit_offset & (kBitsPerByte - 1));
        *bit_mask = (1U << bit_remainder);
        uintptr_t byte_offset = (bit_offset >> kBitsPerByteLog2);
        return ComputeInternalPointer<uint8_t>(byte_offset);
    }

    // Is `address` aligned on a machine word?
    template<typename T> static constexpr bool IsWordAligned(const T* address) {
        // Word alignment in bytes.  Determined from pointer size.
        return IsAligned<kRuntimePointerSize>(address);
    }

    void* pointer_;
    size_t size_;
};

class Label {
public:
    Label() : position_(0) {}

    Label(Label&& src) noexcept : position_(src.position_) {
        // We must unlink/unbind the src label when moving; if not, calling the destructor on
        // the src label would fail.
        src.position_ = 0;
    }

    ~Label() {
        // Assert if label is being destroyed with unresolved branches pending.
        CHECK(!IsLinked());
    }

    // Returns the position for bound and linked labels. Cannot be used
    // for unused labels.
    int Position() const {
        CHECK(!IsUnused());
        return IsBound() ? -position_ - sizeof(void*) : position_ - sizeof(void*);
    }

    int LinkPosition() const {
        CHECK(IsLinked());
        return position_ - sizeof(void*);
    }

    bool IsBound() const { return position_ < 0; }
    bool IsUnused() const { return position_ == 0; }
    bool IsLinked() const { return position_ > 0; }

private:
    int position_;

    void Reinitialize() { position_ = 0; }

    void BindTo(int position) {
        CHECK(!IsBound());
        position_ = -position - sizeof(void*);
        CHECK(IsBound());
    }

    void LinkTo(int position) {
        CHECK(!IsBound());
        position_ = position + sizeof(void*);
        CHECK(IsLinked());
    }

    friend class Riscv64Assembler;
    friend class Riscv64Label;

    DISALLOW_COPY_AND_ASSIGN(Label);
};

// Assembler fixups are positions in generated code that require processing
// after the code has been copied to executable memory. This includes building
// relocation information.
class AssemblerFixup {
public:
    virtual void Process(const MemoryRegion& region, int position) = 0;
    virtual ~AssemblerFixup() {}

private:
    AssemblerFixup* previous_;
    int position_;

    AssemblerFixup* previous() const { return previous_; }
    void set_previous(AssemblerFixup* previous_in) { previous_ = previous_in; }

    int position() const { return position_; }
    void set_position(int position_in) { position_ = position_in; }

    friend class AssemblerBuffer;
};

// Parent of all queued slow paths, emitted during finalization
class SlowPath {
public:
    SlowPath() : next_(nullptr) {}
    virtual ~SlowPath() {}

    Label* Continuation() { return &continuation_; }
    Label* Entry() { return &entry_; }
    // Generate code for slow path
    virtual void Emit(Assembler *sp_asm) = 0;

protected:
    // Entry branched to by fast path
    Label entry_;
    // Optional continuation that is branched to at the end of the slow path
    Label continuation_;
    // Next in linked list of slow paths
    SlowPath *next_;

private:
    friend class AssemblerBuffer;
    DISALLOW_COPY_AND_ASSIGN(SlowPath);
};

class AssemblerBuffer {
public:
    explicit AssemblerBuffer(ArenaAllocator* allocator);
    ~AssemblerBuffer();

    ArenaAllocator* GetAllocator() {
        return allocator_;
    }

    // Basic support for emitting, loading, and storing.
    template<typename T> void Emit(T value) {
        CHECK(HasEnsuredCapacity());
        *reinterpret_cast<T*>(cursor_) = value;
        cursor_ += sizeof(T);
    }

    template<typename T> T Load(size_t position) {
        CHECK_LE(position, Size() - static_cast<int>(sizeof(T)));
        return *reinterpret_cast<T*>(contents_ + position);
    }

    template<typename T> void Store(size_t position, T value) {
        CHECK_LE(position, Size() - static_cast<int>(sizeof(T)));
        *reinterpret_cast<T*>(contents_ + position) = value;
    }

    void Resize(size_t new_size) {
        if (new_size > Capacity()) {
            ExtendCapacity(new_size);
        }
        cursor_ = contents_ + new_size;
    }

    void Move(size_t newposition, size_t oldposition, size_t size) {
        // Move a chunk of the buffer from oldposition to newposition.
        DCHECK_LE(oldposition + size, Size());
        DCHECK_LE(newposition + size, Size());
        memmove(contents_ + newposition, contents_ + oldposition, size);
    }

    // Emit a fixup at the current location.
    void EmitFixup(AssemblerFixup* fixup) {
        fixup->set_previous(fixup_);
        fixup->set_position(Size());
        fixup_ = fixup;
    }

    void EnqueueSlowPath(SlowPath* slowpath) {
        if (slow_path_ == nullptr) {
            slow_path_ = slowpath;
        } else {
            SlowPath* cur = slow_path_;
            for ( ; cur->next_ != nullptr ; cur = cur->next_) {}
            cur->next_ = slowpath;
        }
    }

    void EmitSlowPaths(Assembler* sp_asm) {
        SlowPath* cur = slow_path_;
        SlowPath* next = nullptr;
        slow_path_ = nullptr;
        for ( ; cur != nullptr ; cur = next) {
            cur->Emit(sp_asm);
            next = cur->next_;
            delete cur;
        }
    }

    // Get the size of the emitted code.
    size_t Size() const {
        CHECK_GE(cursor_, contents_);
        return cursor_ - contents_;
    }

    uint8_t* contents() const { return contents_; }

    // Copy the assembled instructions into the specified memory block.
    void CopyInstructions(const MemoryRegion& region);

    // To emit an instruction to the assembler buffer, the EnsureCapacity helper
    // must be used to guarantee that the underlying data area is big enough to
    // hold the emitted instruction. Usage:
    //
    //     AssemblerBuffer buffer;
    //     AssemblerBuffer::EnsureCapacity ensured(&buffer);
    //     ... emit bytes for single instruction ...

#ifndef NDEBUG

    class EnsureCapacity {
    public:
        explicit EnsureCapacity(AssemblerBuffer* buffer) {
            if (buffer->cursor() > buffer->limit()) {
                buffer->ExtendCapacity(buffer->Size() + kMinimumGap);
            }
            // In debug mode, we save the assembler buffer along with the gap
            // size before we start emitting to the buffer. This allows us to
            // check that any single generated instruction doesn't overflow the
            // limit implied by the minimum gap size.
            buffer_ = buffer;
            gap_ = ComputeGap();
            // Make sure that extending the capacity leaves a big enough gap
            // for any kind of instruction.
            CHECK_GE(gap_, kMinimumGap);
            // Mark the buffer as having ensured the capacity.
            CHECK(!buffer->HasEnsuredCapacity());  // Cannot nest.
            buffer->has_ensured_capacity_ = true;
        }

        ~EnsureCapacity() {
            // Unmark the buffer, so we cannot emit after this.
            buffer_->has_ensured_capacity_ = false;
            // Make sure the generated instruction doesn't take up more
            // space than the minimum gap.
            int delta = gap_ - ComputeGap();
            CHECK_LE(delta, kMinimumGap);
        }

    private:
        AssemblerBuffer* buffer_;
        int gap_;

        int ComputeGap() { return buffer_->Capacity() - buffer_->Size(); }
    };

    bool has_ensured_capacity_;
    bool HasEnsuredCapacity() const { return has_ensured_capacity_; }

#else

    class EnsureCapacity {
    public:
        explicit EnsureCapacity(AssemblerBuffer* buffer) {
            if (buffer->cursor() > buffer->limit()) {
                buffer->ExtendCapacity(buffer->Size() + kMinimumGap);
            }
        }
    };

    // When building the C++ tests, assertion code is enabled. To allow
    // asserting that the user of the assembler buffer has ensured the
    // capacity needed for emitting, we add a placeholder method in non-debug mode.
    bool HasEnsuredCapacity() const { return true; }

#endif

    // Returns the position in the instruction stream.
    int GetPosition() { return  cursor_ - contents_; }

    size_t Capacity() const {
        CHECK_GE(limit_, contents_);
        return (limit_ - contents_) + kMinimumGap;
    }

    // Unconditionally increase the capacity.
    // The provided `min_capacity` must be higher than current `Capacity()`.
    void ExtendCapacity(size_t min_capacity);

    void ProcessFixups();

private:
    // The limit is set to kMinimumGap bytes before the end of the data area.
    // This leaves enough space for the longest possible instruction and allows
    // for a single, fast space check per instruction.
    static constexpr int kMinimumGap = 32;

    ArenaAllocator* const allocator_;
    uint8_t* contents_;
    uint8_t* cursor_;
    uint8_t* limit_;
    AssemblerFixup* fixup_;
#ifndef NDEBUG
    bool fixups_processed_;
#endif

    // Head of linked list of slow paths
    SlowPath* slow_path_;

    uint8_t* cursor() const { return cursor_; }
    uint8_t* limit() const { return limit_; }

    // Process the fixup chain starting at the given fixup. The offset is
    // non-zero for fixups in the body if the preamble is non-empty.
    void ProcessFixups(const MemoryRegion& region);

    // Compute the limit based on the data area and the capacity. See
    // description of kMinimumGap for the reasoning behind the value.
    static uint8_t* ComputeLimit(uint8_t* data, size_t capacity) {
        return data + capacity - kMinimumGap;
    }

    friend class AssemblerFixup;
};

struct DebugFrameOpCodeWriterForAssembler {
    int pad{};
};

class Assembler {
public:
    // Finalize the code; emit slow paths, fixup branches, add literal pool, etc.
    virtual void FinalizeCode() {
        buffer_.EmitSlowPaths(this);
        buffer_.ProcessFixups();
    }

    // Size of generated code
    virtual size_t CodeSize() const { return buffer_.Size(); }
    virtual const uint8_t* CodeBufferBaseAddress() const { return buffer_.contents(); }
    // CodePosition() is a non-const method similar to CodeSize(), which is used to
    // record positions within the code buffer for the purpose of signal handling
    // (stack overflow checks and implicit null checks may trigger signals and the
    // signal handlers expect them right before the recorded positions).
    // On most architectures CodePosition() should be equivalent to CodeSize(), but
    // the MIPS assembler needs to be aware of this recording, so it doesn't put
    // the instructions that can trigger signals into branch delay slots. Handling
    // signals from instructions in delay slots is a bit problematic and should be
    // avoided.
    // TODO: Re-evaluate whether we still need this now that MIPS support has been removed.
    virtual size_t CodePosition() { return CodeSize(); }

    // Copy instructions out of assembly buffer into the given region of memory
    //    virtual void CopyInstructions(const MemoryRegion& region) {
    //        buffer_.CopyInstructions(region);
    //    }

    // TODO: Implement with disassembler.
    virtual void Comment([[maybe_unused]] const char* format, ...) {}

    virtual void Bind(Label* label) = 0;
    virtual void Jump(Label* label) = 0;

    virtual ~Assembler() {}

    /**
     * @brief Buffer of DWARF's Call Frame Information opcodes.
     * @details It is used by debuggers and other tools to unwind the call stack.
     */
    DebugFrameOpCodeWriterForAssembler& cfi() { return cfi_; }

    ArenaAllocator* GetAllocator() { return buffer_.GetAllocator(); }

    AssemblerBuffer* GetBuffer() { return &buffer_; }

protected:
    explicit Assembler(ArenaAllocator* allocator) : buffer_(allocator), cfi_() {}

    AssemblerBuffer buffer_;

    DebugFrameOpCodeWriterForAssembler cfi_;
};

class Riscv64Label : public Label {
public:
    Riscv64Label() : prev_branch_id_(kNoPrevBranchId) {}

    Riscv64Label(Riscv64Label&& src) noexcept
            // NOLINTNEXTLINE - src.prev_branch_id_ is valid after the move
            : Label(std::move(src)), prev_branch_id_(src.prev_branch_id_) {}

private:
    static constexpr uint32_t kNoPrevBranchId = std::numeric_limits<uint32_t>::max();

    uint32_t prev_branch_id_;  // To get distance from preceding branch, if any.

    friend class Riscv64Assembler;
    DISALLOW_COPY_AND_ASSIGN(Riscv64Label);
};

// Assembler literal is a value embedded in code, retrieved using a PC-relative load.
class Literal {
public:
    static constexpr size_t kMaxSize = 8;

    Literal(uint32_t size, const uint8_t* data) : label_(), size_(size) {
        DCHECK_LE(size, Literal::kMaxSize);
        memcpy(data_, data, size);
    }

    template <typename T> T GetValue() const {
        DCHECK_EQ(size_, sizeof(T));
        T value;
        memcpy(&value, data_, sizeof(T));
        return value;
    }

    uint32_t GetSize() const { return size_; }

    const uint8_t* GetData() const { return data_; }

    Riscv64Label* GetLabel() { return &label_; }

    const Riscv64Label* GetLabel() const { return &label_; }

private:
    Riscv64Label label_;
    const uint32_t size_;
    uint8_t data_[kMaxSize];

    DISALLOW_COPY_AND_ASSIGN(Literal);
};

// Jump table: table of labels emitted after the code and before the literals. Similar to literals.
class JumpTable {
public:
    explicit JumpTable(ArenaVector<Riscv64Label*>&& labels)
            : label_(), labels_(std::move(labels)) {}

    size_t GetSize() const { return labels_.size() * sizeof(int32_t); }

    const ArenaVector<Riscv64Label*>& GetData() const { return labels_; }

    Riscv64Label* GetLabel() { return &label_; }

    const Riscv64Label* GetLabel() const { return &label_; }

private:
    Riscv64Label label_;
    ArenaVector<Riscv64Label*> labels_;

    DISALLOW_COPY_AND_ASSIGN(JumpTable);
};

class Riscv64Assembler final : public Assembler {
public:
    explicit Riscv64Assembler(ArenaAllocator* allocator)
            : Assembler(allocator)
            , branches_()
            , finalized_(false)
            , overwriting_(false)
            , overwrite_location_(0)
            , literals_()
            , long_literals_()
            , jump_tables_()
            , last_position_adjustment_(0)
            , last_old_position_(0)
            , last_branch_id_(0)
            , available_scratch_core_registers_((1u << TMP) | (1u << TMP2))
            , available_scratch_fp_registers_(1u << FTMP) {
    }

    virtual ~Riscv64Assembler() {
        for (auto& branch : branches_) {
            CHECK(branch.IsResolved());
        }
    }

    size_t CodeSize() const override { return Assembler::CodeSize(); }
    DebugFrameOpCodeWriterForAssembler& cfi() { return Assembler::cfi(); }

    // According to "The RISC-V Instruction Set Manual"

    // LUI/AUIPC (RV32I, with sign-extension on RV64I), opcode = 0x17, 0x37
    // Note: These take a 20-bit unsigned value to align with the clang assembler for testing,
    // but the value stored in the register shall actually be sign-extended to 64 bits.
    void Lui(XRegister rd, uint32_t imm20);
    void Auipc(XRegister rd, uint32_t imm20);

    // Jump instructions (RV32I), opcode = 0x67, 0x6f
    void Jal(XRegister rd, int32_t offset);
    void Jalr(XRegister rd, XRegister rs1, int32_t offset);

    // Branch instructions (RV32I), opcode = 0x63, funct3 from 0x0 ~ 0x1 and 0x4 ~ 0x7
    void Beq(XRegister rs1, XRegister rs2, int32_t offset);
    void Bne(XRegister rs1, XRegister rs2, int32_t offset);
    void Blt(XRegister rs1, XRegister rs2, int32_t offset);
    void Bge(XRegister rs1, XRegister rs2, int32_t offset);
    void Bltu(XRegister rs1, XRegister rs2, int32_t offset);
    void Bgeu(XRegister rs1, XRegister rs2, int32_t offset);

    // Load instructions (RV32I+RV64I): opcode = 0x03, funct3 from 0x0 ~ 0x6
    void Lb(XRegister rd, XRegister rs1, int32_t offset);
    void Lh(XRegister rd, XRegister rs1, int32_t offset);
    void Lw(XRegister rd, XRegister rs1, int32_t offset);
    void Ld(XRegister rd, XRegister rs1, int32_t offset);
    void Lbu(XRegister rd, XRegister rs1, int32_t offset);
    void Lhu(XRegister rd, XRegister rs1, int32_t offset);
    void Lwu(XRegister rd, XRegister rs1, int32_t offset);

    // Store instructions (RV32I+RV64I): opcode = 0x23, funct3 from 0x0 ~ 0x3
    void Sb(XRegister rs2, XRegister rs1, int32_t offset);
    void Sh(XRegister rs2, XRegister rs1, int32_t offset);
    void Sw(XRegister rs2, XRegister rs1, int32_t offset);
    void Sd(XRegister rs2, XRegister rs1, int32_t offset);

    // IMM ALU instructions (RV32I): opcode = 0x13, funct3 from 0x0 ~ 0x7
    void Addi(XRegister rd, XRegister rs1, int32_t imm12);
    void Slti(XRegister rd, XRegister rs1, int32_t imm12);
    void Sltiu(XRegister rd, XRegister rs1, int32_t imm12);
    void Xori(XRegister rd, XRegister rs1, int32_t imm12);
    void Ori(XRegister rd, XRegister rs1, int32_t imm12);
    void Andi(XRegister rd, XRegister rs1, int32_t imm12);
    void Slli(XRegister rd, XRegister rs1, int32_t shamt);
    void Srli(XRegister rd, XRegister rs1, int32_t shamt);
    void Srai(XRegister rd, XRegister rs1, int32_t shamt);

    // ALU instructions (RV32I): opcode = 0x33, funct3 from 0x0 ~ 0x7
    void Add(XRegister rd, XRegister rs1, XRegister rs2);
    void Sub(XRegister rd, XRegister rs1, XRegister rs2);
    void Slt(XRegister rd, XRegister rs1, XRegister rs2);
    void Sltu(XRegister rd, XRegister rs1, XRegister rs2);
    void Xor(XRegister rd, XRegister rs1, XRegister rs2);
    void Or(XRegister rd, XRegister rs1, XRegister rs2);
    void And(XRegister rd, XRegister rs1, XRegister rs2);
    void Sll(XRegister rd, XRegister rs1, XRegister rs2);
    void Srl(XRegister rd, XRegister rs1, XRegister rs2);
    void Sra(XRegister rd, XRegister rs1, XRegister rs2);

    // 32bit Imm ALU instructions (RV64I): opcode = 0x1b, funct3 from 0x0, 0x1, 0x5
    void Addiw(XRegister rd, XRegister rs1, int32_t imm12);
    void Slliw(XRegister rd, XRegister rs1, int32_t shamt);
    void Srliw(XRegister rd, XRegister rs1, int32_t shamt);
    void Sraiw(XRegister rd, XRegister rs1, int32_t shamt);

    // 32bit ALU instructions (RV64I): opcode = 0x3b, funct3 from 0x0 ~ 0x7
    void Addw(XRegister rd, XRegister rs1, XRegister rs2);
    void Subw(XRegister rd, XRegister rs1, XRegister rs2);
    void Sllw(XRegister rd, XRegister rs1, XRegister rs2);
    void Srlw(XRegister rd, XRegister rs1, XRegister rs2);
    void Sraw(XRegister rd, XRegister rs1, XRegister rs2);

    // Environment call and breakpoint (RV32I), opcode = 0x73
    void Ecall();
    void Ebreak();

    // Fence instruction (RV32I): opcode = 0xf, funct3 = 0
    void Fence(uint32_t pred = kFenceDefault, uint32_t succ = kFenceDefault);
    void FenceTso();

    // "Zifencei" Standard Extension, opcode = 0xf, funct3 = 1
    void FenceI();

    // RV32M Standard Extension: opcode = 0x33, funct3 from 0x0 ~ 0x7
    void Mul(XRegister rd, XRegister rs1, XRegister rs2);
    void Mulh(XRegister rd, XRegister rs1, XRegister rs2);
    void Mulhsu(XRegister rd, XRegister rs1, XRegister rs2);
    void Mulhu(XRegister rd, XRegister rs1, XRegister rs2);
    void Div(XRegister rd, XRegister rs1, XRegister rs2);
    void Divu(XRegister rd, XRegister rs1, XRegister rs2);
    void Rem(XRegister rd, XRegister rs1, XRegister rs2);
    void Remu(XRegister rd, XRegister rs1, XRegister rs2);

    // RV64M Standard Extension: opcode = 0x3b, funct3 0x0 and from 0x4 ~ 0x7
    void Mulw(XRegister rd, XRegister rs1, XRegister rs2);
    void Divw(XRegister rd, XRegister rs1, XRegister rs2);
    void Divuw(XRegister rd, XRegister rs1, XRegister rs2);
    void Remw(XRegister rd, XRegister rs1, XRegister rs2);
    void Remuw(XRegister rd, XRegister rs1, XRegister rs2);

    // RV32A/RV64A Standard Extension
    void LrW(XRegister rd, XRegister rs1, AqRl aqrl);
    void LrD(XRegister rd, XRegister rs1, AqRl aqrl);
    void ScW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void ScD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoSwapW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoSwapD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoAddW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoAddD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoXorW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoXorD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoAndW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoAndD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoOrW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoOrD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMinW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMinD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMaxW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMaxD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMinuW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMinuD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMaxuW(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);
    void AmoMaxuD(XRegister rd, XRegister rs2, XRegister rs1, AqRl aqrl);

    // "Zicsr" Standard Extension, opcode = 0x73, funct3 from 0x1 ~ 0x3 and 0x5 ~ 0x7
    void Csrrw(XRegister rd, uint32_t csr, XRegister rs1);
    void Csrrs(XRegister rd, uint32_t csr, XRegister rs1);
    void Csrrc(XRegister rd, uint32_t csr, XRegister rs1);
    void Csrrwi(XRegister rd, uint32_t csr, uint32_t uimm5);
    void Csrrsi(XRegister rd, uint32_t csr, uint32_t uimm5);
    void Csrrci(XRegister rd, uint32_t csr, uint32_t uimm5);

    // FP load/store instructions (RV32F+RV32D): opcode = 0x07, 0x27
    void FLw(FRegister rd, XRegister rs1, int32_t offset);
    void FLd(FRegister rd, XRegister rs1, int32_t offset);
    void FSw(FRegister rs2, XRegister rs1, int32_t offset);
    void FSd(FRegister rs2, XRegister rs1, int32_t offset);

    // FP FMA instructions (RV32F+RV32D): opcode = 0x43, 0x47, 0x4b, 0x4f
    void FMAddS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FMAddD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FMSubS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FMSubD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FNMSubS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FNMSubD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FNMAddS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);
    void FNMAddD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3, FPRoundingMode frm);

    // FP FMA instruction helpers passing the default rounding mode.
    void FMAddS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FMAddS(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FMAddD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FMAddD(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FMSubS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FMSubS(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FMSubD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FMSubD(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FNMSubS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FNMSubS(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FNMSubD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FNMSubD(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FNMAddS(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FNMAddS(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }
    void FNMAddD(FRegister rd, FRegister rs1, FRegister rs2, FRegister rs3) {
        FNMAddD(rd, rs1, rs2, rs3, FPRoundingMode::kDefault);
    }

    // Simple FP instructions (RV32F+RV32D): opcode = 0x53, funct7 = 0b0XXXX0D
    void FAddS(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FAddD(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FSubS(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FSubD(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FMulS(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FMulD(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FDivS(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FDivD(FRegister rd, FRegister rs1, FRegister rs2, FPRoundingMode frm);
    void FSqrtS(FRegister rd, FRegister rs1, FPRoundingMode frm);
    void FSqrtD(FRegister rd, FRegister rs1, FPRoundingMode frm);
    void FSgnjS(FRegister rd, FRegister rs1, FRegister rs2);
    void FSgnjD(FRegister rd, FRegister rs1, FRegister rs2);
    void FSgnjnS(FRegister rd, FRegister rs1, FRegister rs2);
    void FSgnjnD(FRegister rd, FRegister rs1, FRegister rs2);
    void FSgnjxS(FRegister rd, FRegister rs1, FRegister rs2);
    void FSgnjxD(FRegister rd, FRegister rs1, FRegister rs2);
    void FMinS(FRegister rd, FRegister rs1, FRegister rs2);
    void FMinD(FRegister rd, FRegister rs1, FRegister rs2);
    void FMaxS(FRegister rd, FRegister rs1, FRegister rs2);
    void FMaxD(FRegister rd, FRegister rs1, FRegister rs2);
    void FCvtSD(FRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtDS(FRegister rd, FRegister rs1, FPRoundingMode frm);

    // Simple FP instruction helpers passing the default rounding mode.
    void FAddS(FRegister rd, FRegister rs1, FRegister rs2) {
        FAddS(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FAddD(FRegister rd, FRegister rs1, FRegister rs2) {
        FAddD(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FSubS(FRegister rd, FRegister rs1, FRegister rs2) {
        FSubS(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FSubD(FRegister rd, FRegister rs1, FRegister rs2) {
        FSubD(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FMulS(FRegister rd, FRegister rs1, FRegister rs2) {
        FMulS(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FMulD(FRegister rd, FRegister rs1, FRegister rs2) {
        FMulD(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FDivS(FRegister rd, FRegister rs1, FRegister rs2) {
        FDivS(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FDivD(FRegister rd, FRegister rs1, FRegister rs2) {
        FDivD(rd, rs1, rs2, FPRoundingMode::kDefault);
    }
    void FSqrtS(FRegister rd, FRegister rs1) { FSqrtS(rd, rs1, FPRoundingMode::kDefault); }
    void FSqrtD(FRegister rd, FRegister rs1) { FSqrtD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtSD(FRegister rd, FRegister rs1) { FCvtSD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtDS(FRegister rd, FRegister rs1) { FCvtDS(rd, rs1, FPRoundingMode::kIgnored); }

    // FP compare instructions (RV32F+RV32D): opcode = 0x53, funct7 = 0b101000D
    void FEqS(XRegister rd, FRegister rs1, FRegister rs2);
    void FEqD(XRegister rd, FRegister rs1, FRegister rs2);
    void FLtS(XRegister rd, FRegister rs1, FRegister rs2);
    void FLtD(XRegister rd, FRegister rs1, FRegister rs2);
    void FLeS(XRegister rd, FRegister rs1, FRegister rs2);
    void FLeD(XRegister rd, FRegister rs1, FRegister rs2);

    // FP conversion instructions (RV32F+RV32D+RV64F+RV64D): opcode = 0x53, funct7 = 0b110X00D
    void FCvtWS(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtWD(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtWuS(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtWuD(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtLS(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtLD(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtLuS(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtLuD(XRegister rd, FRegister rs1, FPRoundingMode frm);
    void FCvtSW(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtDW(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtSWu(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtDWu(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtSL(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtDL(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtSLu(FRegister rd, XRegister rs1, FPRoundingMode frm);
    void FCvtDLu(FRegister rd, XRegister rs1, FPRoundingMode frm);

    // FP conversion instruction helpers passing the default rounding mode.
    void FCvtWS(XRegister rd, FRegister rs1) { FCvtWS(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtWD(XRegister rd, FRegister rs1) { FCvtWD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtWuS(XRegister rd, FRegister rs1) { FCvtWuS(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtWuD(XRegister rd, FRegister rs1) { FCvtWuD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtLS(XRegister rd, FRegister rs1) { FCvtLS(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtLD(XRegister rd, FRegister rs1) { FCvtLD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtLuS(XRegister rd, FRegister rs1) { FCvtLuS(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtLuD(XRegister rd, FRegister rs1) { FCvtLuD(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtSW(FRegister rd, XRegister rs1) { FCvtSW(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtDW(FRegister rd, XRegister rs1) { FCvtDW(rd, rs1, FPRoundingMode::kIgnored); }
    void FCvtSWu(FRegister rd, XRegister rs1) { FCvtSWu(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtDWu(FRegister rd, XRegister rs1) { FCvtDWu(rd, rs1, FPRoundingMode::kIgnored); }
    void FCvtSL(FRegister rd, XRegister rs1) { FCvtSL(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtDL(FRegister rd, XRegister rs1) { FCvtDL(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtSLu(FRegister rd, XRegister rs1) { FCvtSLu(rd, rs1, FPRoundingMode::kDefault); }
    void FCvtDLu(FRegister rd, XRegister rs1) { FCvtDLu(rd, rs1, FPRoundingMode::kDefault); }

    // FP move instructions (RV32F+RV32D): opcode = 0x53, funct3 = 0x0, funct7 = 0b111X00D
    void FMvXW(XRegister rd, FRegister rs1);
    void FMvXD(XRegister rd, FRegister rs1);
    void FMvWX(FRegister rd, XRegister rs1);
    void FMvDX(FRegister rd, XRegister rs1);

    // FP classify instructions (RV32F+RV32D): opcode = 0x53, funct3 = 0x1, funct7 = 0b111X00D
    void FClassS(XRegister rd, FRegister rs1);
    void FClassD(XRegister rd, FRegister rs1);

    // "Zba" Standard Extension, opcode = 0x1b, 0x33 or 0x3b, funct3 and funct7 varies.
    void AddUw(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh1Add(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh1AddUw(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh2Add(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh2AddUw(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh3Add(XRegister rd, XRegister rs1, XRegister rs2);
    void Sh3AddUw(XRegister rd, XRegister rs1, XRegister rs2);
    void SlliUw(XRegister rd, XRegister rs1, int32_t shamt);

    // "Zbb" Standard Extension, opcode = 0x13, 0x1b or 0x33, funct3 and funct7 varies.
    // Note: We do not support 32-bit sext.b, sext.h and zext.h from the Zbb extension.
    // (Neither does the clang-r498229's assembler which we currently test against.)
    void Andn(XRegister rd, XRegister rs1, XRegister rs2);
    void Orn(XRegister rd, XRegister rs1, XRegister rs2);
    void Xnor(XRegister rd, XRegister rs1, XRegister rs2);
    void Clz(XRegister rd, XRegister rs1);
    void Clzw(XRegister rd, XRegister rs1);
    void Ctz(XRegister rd, XRegister rs1);
    void Ctzw(XRegister rd, XRegister rs1);
    void Cpop(XRegister rd, XRegister rs1);
    void Cpopw(XRegister rd, XRegister rs1);
    void Min(XRegister rd, XRegister rs1, XRegister rs2);
    void Minu(XRegister rd, XRegister rs1, XRegister rs2);
    void Max(XRegister rd, XRegister rs1, XRegister rs2);
    void Maxu(XRegister rd, XRegister rs1, XRegister rs2);
    void Rol(XRegister rd, XRegister rs1, XRegister rs2);
    void Rolw(XRegister rd, XRegister rs1, XRegister rs2);
    void Ror(XRegister rd, XRegister rs1, XRegister rs2);
    void Rorw(XRegister rd, XRegister rs1, XRegister rs2);
    void Rori(XRegister rd, XRegister rs1, int32_t shamt);
    void Roriw(XRegister rd, XRegister rs1, int32_t shamt);
    void OrcB(XRegister rd, XRegister rs1);
    void Rev8(XRegister rd, XRegister rs1);

    ////////////////////////////// RV64 MACRO Instructions  START ///////////////////////////////
    // These pseudo instructions are from "RISC-V Assembly Programmer's Manual".

    void Nop();
    void Li(XRegister rd, int64_t imm);
    void Mv(XRegister rd, XRegister rs);
    void Not(XRegister rd, XRegister rs);
    void Neg(XRegister rd, XRegister rs);
    void NegW(XRegister rd, XRegister rs);
    void SextB(XRegister rd, XRegister rs);
    void SextH(XRegister rd, XRegister rs);
    void SextW(XRegister rd, XRegister rs);
    void ZextB(XRegister rd, XRegister rs);
    void ZextH(XRegister rd, XRegister rs);
    void ZextW(XRegister rd, XRegister rs);
    void Seqz(XRegister rd, XRegister rs);
    void Snez(XRegister rd, XRegister rs);
    void Sltz(XRegister rd, XRegister rs);
    void Sgtz(XRegister rd, XRegister rs);
    void FMvS(FRegister rd, FRegister rs);
    void FAbsS(FRegister rd, FRegister rs);
    void FNegS(FRegister rd, FRegister rs);
    void FMvD(FRegister rd, FRegister rs);
    void FAbsD(FRegister rd, FRegister rs);
    void FNegD(FRegister rd, FRegister rs);

    // Branch pseudo instructions
    void Beqz(XRegister rs, int32_t offset);
    void Bnez(XRegister rs, int32_t offset);
    void Blez(XRegister rs, int32_t offset);
    void Bgez(XRegister rs, int32_t offset);
    void Bltz(XRegister rs, int32_t offset);
    void Bgtz(XRegister rs, int32_t offset);
    void Bgt(XRegister rs, XRegister rt, int32_t offset);
    void Ble(XRegister rs, XRegister rt, int32_t offset);
    void Bgtu(XRegister rs, XRegister rt, int32_t offset);
    void Bleu(XRegister rs, XRegister rt, int32_t offset);

    // Jump pseudo instructions
    void J(int32_t offset);
    void Jal(int32_t offset);
    void Jr(XRegister rs);
    void Jalr(XRegister rs);
    void Jalr(XRegister rd, XRegister rs);
    void Ret();

    // Pseudo instructions for accessing control and status registers
    void RdCycle(XRegister rd);
    void RdTime(XRegister rd);
    void RdInstret(XRegister rd);
    void Csrr(XRegister rd, uint32_t csr);
    void Csrw(uint32_t csr, XRegister rs);
    void Csrs(uint32_t csr, XRegister rs);
    void Csrc(uint32_t csr, XRegister rs);
    void Csrwi(uint32_t csr, uint32_t uimm5);
    void Csrsi(uint32_t csr, uint32_t uimm5);
    void Csrci(uint32_t csr, uint32_t uimm5);

    // Load/store macros for arbitrary 32-bit offsets.
    void Loadb(XRegister rd, XRegister rs1, int32_t offset);
    void Loadh(XRegister rd, XRegister rs1, int32_t offset);
    void Loadw(XRegister rd, XRegister rs1, int32_t offset);
    void Loadd(XRegister rd, XRegister rs1, int32_t offset);
    void Loadbu(XRegister rd, XRegister rs1, int32_t offset);
    void Loadhu(XRegister rd, XRegister rs1, int32_t offset);
    void Loadwu(XRegister rd, XRegister rs1, int32_t offset);
    void Storeb(XRegister rs2, XRegister rs1, int32_t offset);
    void Storeh(XRegister rs2, XRegister rs1, int32_t offset);
    void Storew(XRegister rs2, XRegister rs1, int32_t offset);
    void Stored(XRegister rs2, XRegister rs1, int32_t offset);
    void FLoadw(FRegister rd, XRegister rs1, int32_t offset);
    void FLoadd(FRegister rd, XRegister rs1, int32_t offset);
    void FStorew(FRegister rs2, XRegister rs1, int32_t offset);
    void FStored(FRegister rs2, XRegister rs1, int32_t offset);

    // Macros for loading constants.
    void LoadConst32(XRegister rd, int32_t value);
    void LoadConst64(XRegister rd, int64_t value);

    // Macros for adding constants.
    void AddConst32(XRegister rd, XRegister rs1, int32_t value);
    void AddConst64(XRegister rd, XRegister rs1, int64_t value);

    // Jumps and branches to a label.
    void Beqz(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Bnez(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Blez(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Bgez(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Bltz(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Bgtz(XRegister rs, Riscv64Label* label, bool is_bare = false);
    void Beq(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bne(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Ble(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bge(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Blt(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bgt(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bleu(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bgeu(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bltu(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Bgtu(XRegister rs, XRegister rt, Riscv64Label* label, bool is_bare = false);
    void Jal(XRegister rd, Riscv64Label* label, bool is_bare = false);
    void J(Riscv64Label* label, bool is_bare = false);
    void Jal(Riscv64Label* label, bool is_bare = false);

    // Literal load.
    void Loadw(XRegister rd, Literal* literal);
    void Loadwu(XRegister rd, Literal* literal);
    void Loadd(XRegister rd, Literal* literal);
    void FLoadw(FRegister rd, Literal* literal);
    void FLoadd(FRegister rd, Literal* literal);

    // Illegal instruction that triggers SIGILL.
    void Unimp();

    /////////////////////////////// RV64 MACRO Instructions END ///////////////////////////////

    void Bind(Label* label) override { Bind(down_cast<Riscv64Label*>(label)); }

    void Jump([[maybe_unused]] Label* label) override { assert(false); }

    void Jump(Riscv64Label* label) { J(label); }

    void Bind(Riscv64Label* label);

    // Load label address using PC-relative loads.
    void LoadLabelAddress(XRegister rd, Riscv64Label* label);

    // Create a new literal with a given value.
    // NOTE:Use `Identity<>` to force the template parameter to be explicitly specified.
    template <typename T> Literal* NewLiteral(typename Identity<T>::type value) {
        static_assert(std::is_integral<T>::value, "T must be an integral type.");
        return NewLiteral(sizeof(value), reinterpret_cast<const uint8_t*>(&value));
    }

    // Create a new literal with the given data.
    Literal* NewLiteral(size_t size, const uint8_t* data);

    // Create a jump table for the given labels that will be emitted when finalizing.
    // When the table is emitted, offsets will be relative to the location of the table.
    // The table location is determined by the location of its label (the label precedes
    // the table data) and should be loaded using LoadLabelAddress().
    JumpTable* CreateJumpTable(ArenaVector<Riscv64Label*>&& labels);

public:
    // Emit slow paths queued during assembly, promote short branches to long if needed,
    // and emit branches.
    void FinalizeCode() override;

    // Returns the current location of a label.
    //
    // This function must be used instead of `Riscv64Label::GetPosition()`
    // which returns assembler's internal data instead of an actual location.
    //
    // The location can change during branch fixup in `FinalizeCode()`. Before that,
    // the location is not final and therefore not very useful to external users,
    // so they should preferably retrieve the location only after `FinalizeCode()`.
    uint32_t GetLabelLocation(const Riscv64Label* label) const;

    // Get the final position of a label after local fixup based on the old position
    // recorded before FinalizeCode().
    uint32_t GetAdjustedPosition(uint32_t old_position);

private:
    enum BranchCondition : uint8_t {
        kCondEQ,
        kCondNE,
        kCondLT,
        kCondGE,
        kCondLE,
        kCondGT,
        kCondLTU,
        kCondGEU,
        kCondLEU,
        kCondGTU,
        kUncond,
    };

    // Note that PC-relative literal loads are handled as pseudo branches because they need
    // to be emitted after branch relocation to use correct offsets.
    class Branch {
    public:
        enum Type : uint8_t {
            // TODO(riscv64): Support 16-bit instructions ("C" Standard Extension).

            // Short branches (can be promoted to longer).
            kCondBranch,
            kUncondBranch,
            kCall,
            // Short branches (can't be promoted to longer).
            kBareCondBranch,
            kBareUncondBranch,
            kBareCall,

            // Medium branch (can be promoted to long).
            kCondBranch21,

            // Long branches.
            kLongCondBranch,
            kLongUncondBranch,
            kLongCall,

            // Label.
            kLabel,

            // Literals.
            kLiteral,
            kLiteralUnsigned,
            kLiteralLong,
            kLiteralFloat,
            kLiteralDouble,
        };

        // Bit sizes of offsets defined as enums to minimize chance of typos.
        enum OffsetBits {
            kOffset13 = 13,
            kOffset21 = 21,
            kOffset32 = 32,
        };

        static constexpr uint32_t kUnresolved = 0xffffffff;  // Unresolved target_
        static constexpr uint32_t kMaxBranchLength = 12;     // In bytes.

        struct BranchInfo {
            // Branch length in bytes.
            uint32_t length;
            // The offset in bytes of the PC used in the (only) PC-relative instruction from
            // the start of the branch sequence. RISC-V always uses the address of the PC-relative
            // instruction as the PC, so this is essentially the offset of that instruction.
            uint32_t pc_offset;
            // How large (in bits) a PC-relative offset can be for a given type of branch.
            OffsetBits offset_size;
        };
        static const BranchInfo branch_info_[/* Type */];

        // Unconditional branch or call.
        Branch(uint32_t location, uint32_t target, XRegister rd, bool is_bare);
        // Conditional branch.
        Branch(uint32_t location,
               uint32_t target,
               BranchCondition condition,
               XRegister lhs_reg,
               XRegister rhs_reg,
               bool is_bare);
        // Label address or literal.
        Branch(uint32_t location, uint32_t target, XRegister rd, Type label_or_literal_type);
        Branch(uint32_t location, uint32_t target, FRegister rd, Type literal_type);

        // Some conditional branches with lhs = rhs are effectively NOPs, while some
        // others are effectively unconditional.
        static bool IsNop(BranchCondition condition, XRegister lhs, XRegister rhs);
        static bool IsUncond(BranchCondition condition, XRegister lhs, XRegister rhs);

        static BranchCondition OppositeCondition(BranchCondition cond);

        Type GetType() const;
        BranchCondition GetCondition() const;
        XRegister GetLeftRegister() const;
        XRegister GetRightRegister() const;
        FRegister GetFRegister() const;
        uint32_t GetTarget() const;
        uint32_t GetLocation() const;
        uint32_t GetOldLocation() const;
        uint32_t GetLength() const;
        uint32_t GetOldLength() const;
        uint32_t GetEndLocation() const;
        uint32_t GetOldEndLocation() const;
        bool IsBare() const;
        bool IsResolved() const;

        // Returns the bit size of the signed offset that the branch instruction can handle.
        OffsetBits GetOffsetSize() const;

        // Calculates the distance between two byte locations in the assembler buffer and
        // returns the number of bits needed to represent the distance as a signed integer.
        static OffsetBits GetOffsetSizeNeeded(uint32_t location, uint32_t target);

        // Resolve a branch when the target is known.
        void Resolve(uint32_t target);

        // Relocate a branch by a given delta if needed due to expansion of this or another
        // branch at a given location by this delta (just changes location_ and target_).
        void Relocate(uint32_t expand_location, uint32_t delta);

        // If necessary, updates the type by promoting a short branch to a longer branch
        // based on the branch location and target. Returns the amount (in bytes) by
        // which the branch size has increased.
        uint32_t PromoteIfNeeded();

        // Returns the offset into assembler buffer that shall be used as the base PC for
        // offset calculation. RISC-V always uses the address of the PC-relative instruction
        // as the PC, so this is essentially the location of that instruction.
        uint32_t GetOffsetLocation() const;

        // Calculates and returns the offset ready for encoding in the branch instruction(s).
        int32_t GetOffset() const;

    private:
        // Completes branch construction by determining and recording its type.
        void InitializeType(Type initial_type);
        // Helper for the above.
        void InitShortOrLong(OffsetBits ofs_size,
                             Type short_type,
                             Type long_type,
                             Type longest_type);

        uint32_t old_location_;  // Offset into assembler buffer in bytes.
        uint32_t location_;      // Offset into assembler buffer in bytes.
        uint32_t target_;        // Offset into assembler buffer in bytes.

        XRegister lhs_reg_;          // Left-hand side register in conditional branches or
                                     // destination register in calls or literals.
        XRegister rhs_reg_;          // Right-hand side register in conditional branches.
        FRegister freg_;             // Destination register in FP literals.
        BranchCondition condition_;  // Condition for conditional branches.

        Type type_;      // Current type of the branch.
        Type old_type_;  // Initial type of the branch.
    };

    // Branch and literal fixup.

    void EmitBcond(BranchCondition cond, XRegister rs, XRegister rt, int32_t offset);
    void EmitBranch(Branch* branch);
    void EmitBranches();
    void EmitJumpTables();
    void EmitLiterals();

    void FinalizeLabeledBranch(Riscv64Label* label);
    void Bcond(Riscv64Label* label,
               bool is_bare,
               BranchCondition condition,
               XRegister lhs,
               XRegister rhs);
    void Buncond(Riscv64Label* label, XRegister rd, bool is_bare);
    template <typename XRegisterOrFRegister>
    void LoadLiteral(Literal* literal, XRegisterOrFRegister rd, Branch::Type literal_type);

    Branch* GetBranch(uint32_t branch_id);
    const Branch* GetBranch(uint32_t branch_id) const;

    void ReserveJumpTableSpace();
    void PromoteBranches();
    void PatchCFI();

    // Emit data (e.g. encoded instruction or immediate) to the instruction stream.
    void Emit(uint32_t value);

    // Adjust base register and offset if needed for load/store with a large offset.
    void AdjustBaseAndOffset(XRegister& base, int32_t& offset, ScratchRegisterScope& srs);

    // Helper templates for loads/stores with 32-bit offsets.
    template <void (Riscv64Assembler::*insn)(XRegister, XRegister, int32_t)>
    void LoadFromOffset(XRegister rd, XRegister rs1, int32_t offset);
    template <void (Riscv64Assembler::*insn)(XRegister, XRegister, int32_t)>
    void StoreToOffset(XRegister rs2, XRegister rs1, int32_t offset);
    template <void (Riscv64Assembler::*insn)(FRegister, XRegister, int32_t)>
    void FLoadFromOffset(FRegister rd, XRegister rs1, int32_t offset);
    template <void (Riscv64Assembler::*insn)(FRegister, XRegister, int32_t)>
    void FStoreToOffset(FRegister rs2, XRegister rs1, int32_t offset);

    // Implementation helper for `Li()`, `LoadConst32()` and `LoadConst64()`.
    void LoadImmediate(XRegister rd, int64_t imm, bool can_use_tmp);

    // Emit helpers.

    // I-type instruction:
    //
    //    31                   20 19     15 14 12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . . . . . . . . | . . . . | . . | . . . . | . . . . . . ]
    //   [        imm11:0            rs1   funct3     rd        opcode   ]
    //   -----------------------------------------------------------------
    template <typename Reg1, typename Reg2>
    void EmitI(int32_t imm12, Reg1 rs1, uint32_t funct3, Reg2 rd, uint32_t opcode) {
        DCHECK(IsInt<12>(imm12));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = static_cast<uint32_t>(imm12) << 20 | static_cast<uint32_t>(rs1) << 15 |
                            funct3 << 12 | static_cast<uint32_t>(rd) << 7 | opcode;
        Emit(encoding);
    }

    // R-type instruction:
    //
    //    31         25 24     20 19     15 14 12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . . . | . . . . | . . . . | . . | . . . . | . . . . . . ]
    //   [   funct7        rs2       rs1   funct3     rd        opcode   ]
    //   -----------------------------------------------------------------
    template <typename Reg1, typename Reg2, typename Reg3>
    void EmitR(uint32_t funct7, Reg1 rs2, Reg2 rs1, uint32_t funct3, Reg3 rd, uint32_t opcode) {
        DCHECK(IsUint<7>(funct7));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs2)));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = funct7 << 25 | static_cast<uint32_t>(rs2) << 20 |
                            static_cast<uint32_t>(rs1) << 15 | funct3 << 12 |
                            static_cast<uint32_t>(rd) << 7 | opcode;
        Emit(encoding);
    }

    // R-type instruction variant for floating-point fused multiply-add/sub (F[N]MADD/ F[N]MSUB):
    //
    //    31     27  25 24     20 19     15 14 12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . | . | . . . . | . . . . | . . | . . . . | . . . . . . ]
    //   [  rs3     fmt    rs2       rs1   funct3     rd        opcode   ]
    //   -----------------------------------------------------------------
    template <typename Reg1, typename Reg2, typename Reg3, typename Reg4> void EmitR4(
            Reg1 rs3, uint32_t fmt, Reg2 rs2, Reg3 rs1, uint32_t funct3, Reg4 rd, uint32_t opcode) {
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs3)));
        DCHECK(IsUint<2>(fmt));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs2)));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = static_cast<uint32_t>(rs3) << 27 | static_cast<uint32_t>(fmt) << 25 |
                            static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
                            static_cast<uint32_t>(funct3) << 12 | static_cast<uint32_t>(rd) << 7 |
                            opcode;
        Emit(encoding);
    }

    // S-type instruction:
    //
    //    31         25 24     20 19     15 14 12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . . . | . . . . | . . . . | . . | . . . . | . . . . . . ]
    //   [   imm11:5       rs2       rs1   funct3   imm4:0      opcode   ]
    //   -----------------------------------------------------------------
    template <typename Reg1, typename Reg2>
    void EmitS(int32_t imm12, Reg1 rs2, Reg2 rs1, uint32_t funct3, uint32_t opcode) {
        DCHECK(IsInt<12>(imm12));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs2)));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = (static_cast<uint32_t>(imm12) & 0xFE0) << 20 |
                            static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
                            static_cast<uint32_t>(funct3) << 12 |
                            (static_cast<uint32_t>(imm12) & 0x1F) << 7 | opcode;
        Emit(encoding);
    }

    // I-type instruction variant for shifts (SLLI / SRLI / SRAI):
    //
    //    31       26 25       20 19     15 14 12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . . | . . . . . | . . . . | . . | . . . . | . . . . . . ]
    //   [  imm11:6  imm5:0(shamt)   rs1   funct3     rd        opcode   ]
    //   -----------------------------------------------------------------
    void EmitI6(uint32_t funct6,
                uint32_t imm6,
                XRegister rs1,
                uint32_t funct3,
                XRegister rd,
                uint32_t opcode) {
        DCHECK(IsUint<6>(funct6));
        DCHECK(IsUint<6>(imm6));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = funct6 << 26 | static_cast<uint32_t>(imm6) << 20 |
                            static_cast<uint32_t>(rs1) << 15 | funct3 << 12 |
                            static_cast<uint32_t>(rd) << 7 | opcode;
        Emit(encoding);
    }

    // B-type instruction:
    //
    //   31 30       25 24     20 19     15 14 12 11    8 7 6           0
    //   -----------------------------------------------------------------
    //   [ | . . . . . | . . . . | . . . . | . . | . . . | | . . . . . . ]
    //  imm12 imm11:5      rs2       rs1   funct3 imm4:1 imm11  opcode   ]
    //   -----------------------------------------------------------------
    void EmitB(int32_t offset, XRegister rs2, XRegister rs1, uint32_t funct3, uint32_t opcode) {
        DCHECK_ALIGNED(offset, 2);
        DCHECK(IsInt<13>(offset));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs2)));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rs1)));
        DCHECK(IsUint<3>(funct3));
        DCHECK(IsUint<7>(opcode));
        uint32_t imm12 = (static_cast<uint32_t>(offset) >> 1) & 0xfffu;
        uint32_t encoding = (imm12 & 0x800u) << (31 - 11) | (imm12 & 0x03f0u) << (25 - 4) |
                            static_cast<uint32_t>(rs2) << 20 | static_cast<uint32_t>(rs1) << 15 |
                            static_cast<uint32_t>(funct3) << 12 | (imm12 & 0xfu) << 8 |
                            (imm12 & 0x400u) >> (10 - 7) | opcode;
        Emit(encoding);
    }

    // U-type instruction:
    //
    //    31                                   12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ . . . . . . . . . . . . . . . . . . . | . . . . | . . . . . . ]
    //   [                imm31:12                    rd        opcode   ]
    //   -----------------------------------------------------------------
    void EmitU(uint32_t imm20, XRegister rd, uint32_t opcode) {
        CHECK(IsUint<20>(imm20));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t encoding = imm20 << 12 | static_cast<uint32_t>(rd) << 7 | opcode;
        Emit(encoding);
    }

    // J-type instruction:
    //
    //   31 30               21   19           12 11      7 6           0
    //   -----------------------------------------------------------------
    //   [ | . . . . . . . . . | | . . . . . . . | . . . . | . . . . . . ]
    //  imm20    imm10:1      imm11   imm19:12        rd        opcode   ]
    //   -----------------------------------------------------------------
    void EmitJ(int32_t offset, XRegister rd, uint32_t opcode) {
        DCHECK_ALIGNED(offset, 2);
        CHECK(IsInt<21>(offset));
        DCHECK(IsUint<5>(static_cast<uint32_t>(rd)));
        DCHECK(IsUint<7>(opcode));
        uint32_t imm20 = (static_cast<uint32_t>(offset) >> 1) & 0xfffffu;
        uint32_t encoding = (imm20 & 0x80000u) << (31 - 19) | (imm20 & 0x03ffu) << 21 |
                            (imm20 & 0x400u) << (20 - 10) | (imm20 & 0x7f800u) << (12 - 11) |
                            static_cast<uint32_t>(rd) << 7 | opcode;
        Emit(encoding);
    }

    ArenaVector<Branch> branches_;

    // For checking that we finalize the code only once.
    bool finalized_;

    // Whether appending instructions at the end of the buffer or overwriting the existing ones.
    bool overwriting_;
    // The current overwrite location.
    uint32_t overwrite_location_;

    // Use `std::deque<>` for literal labels to allow insertions at the end
    // without invalidating pointers and references to existing elements.
    ArenaDeque<Literal> literals_;
    ArenaDeque<Literal> long_literals_;  // 64-bit literals separated for alignment reasons.

    // Jump table list.
    ArenaDeque<JumpTable> jump_tables_;

    // Data for `GetAdjustedPosition()`, see the description there.
    uint32_t last_position_adjustment_;
    uint32_t last_old_position_;
    uint32_t last_branch_id_;

    uint32_t available_scratch_core_registers_;
    uint32_t available_scratch_fp_registers_;

    static constexpr uint32_t kXlen = 64;

    friend class ScratchRegisterScope;

    DISALLOW_COPY_AND_ASSIGN(Riscv64Assembler);
};

class ScratchRegisterScope {
public:
    explicit ScratchRegisterScope(Riscv64Assembler* assembler)
            : assembler_(assembler)
            , old_available_scratch_core_registers_(assembler->available_scratch_core_registers_)
            , old_available_scratch_fp_registers_(assembler->available_scratch_fp_registers_) {}

    ~ScratchRegisterScope() {
        assembler_->available_scratch_core_registers_ = old_available_scratch_core_registers_;
        assembler_->available_scratch_fp_registers_ = old_available_scratch_fp_registers_;
    }

    // Alocate a scratch `XRegister`. There must be an available register to allocate.
    XRegister AllocateXRegister() {
        CHECK_NE(assembler_->available_scratch_core_registers_, 0u);
        // Allocate the highest available scratch register (prefer TMP(T6) over TMP2(T5)).
        uint32_t reg_num = (BitSizeOf(assembler_->available_scratch_core_registers_) - 1u) -
                           CLZ(assembler_->available_scratch_core_registers_);
        assembler_->available_scratch_core_registers_ &= ~(1u << reg_num);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfXRegisters));
        return enum_cast<XRegister>(reg_num);
    }

    // Free a previously unavailable core register for use as a scratch register.
    // This can be an arbitrary register, not necessarly the usual `TMP` or `TMP2`.
    void FreeXRegister(XRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfXRegisters));
        CHECK_EQ((1u << reg_num) & assembler_->available_scratch_core_registers_, 0u);
        assembler_->available_scratch_core_registers_ |= 1u << reg_num;
    }

    // The number of available scratch core registers.
    size_t AvailableXRegisters() { return POPCOUNT(assembler_->available_scratch_core_registers_); }

    // Make sure a core register is available for use as a scratch register.
    void IncludeXRegister(XRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfXRegisters));
        assembler_->available_scratch_core_registers_ |= 1u << reg_num;
    }

    // Make sure a core register is not available for use as a scratch register.
    void ExcludeXRegister(XRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfXRegisters));
        assembler_->available_scratch_core_registers_ &= ~(1u << reg_num);
    }

    // Alocate a scratch `FRegister`. There must be an available register to allocate.
    FRegister AllocateFRegister() {
        CHECK_NE(assembler_->available_scratch_fp_registers_, 0u);
        // Allocate the highest available scratch register (same as for core registers).
        uint32_t reg_num = (BitSizeOf(assembler_->available_scratch_fp_registers_) - 1u) -
                           CLZ(assembler_->available_scratch_fp_registers_);
        assembler_->available_scratch_fp_registers_ &= ~(1u << reg_num);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfFRegisters));
        return enum_cast<FRegister>(reg_num);
    }

    // Free a previously unavailable FP register for use as a scratch register.
    // This can be an arbitrary register, not necessarly the usual `FTMP`.
    void FreeFRegister(FRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfFRegisters));
        CHECK_EQ((1u << reg_num) & assembler_->available_scratch_fp_registers_, 0u);
        assembler_->available_scratch_fp_registers_ |= 1u << reg_num;
    }

    // The number of available scratch FP registers.
    size_t AvailableFRegisters() { return POPCOUNT(assembler_->available_scratch_fp_registers_); }

    // Make sure an FP register is available for use as a scratch register.
    void IncludeFRegister(FRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfFRegisters));
        assembler_->available_scratch_fp_registers_ |= 1u << reg_num;
    }

    // Make sure an FP register is not available for use as a scratch register.
    void ExcludeFRegister(FRegister reg) {
        uint32_t reg_num = enum_cast<uint32_t>(reg);
        DCHECK_LT(reg_num, enum_cast<uint32_t>(kNumberOfFRegisters));
        assembler_->available_scratch_fp_registers_ &= ~(1u << reg_num);
    }

private:
    Riscv64Assembler* const assembler_;
    const uint32_t old_available_scratch_core_registers_;
    const uint32_t old_available_scratch_fp_registers_;

    DISALLOW_COPY_AND_ASSIGN(ScratchRegisterScope);
};

}  // namespace riscv64
}  // namespace art
