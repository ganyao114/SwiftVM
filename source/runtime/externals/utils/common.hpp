/*
 * Copyright (c) Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once

#if defined(__clang__)
#define ATMOSPHERE_COMPILER_CLANG
#elif defined(__GNUG__) || defined(__GNUC__)
#define ATMOSPHERE_COMPILER_GCC
#else
#error "Unknown compiler!"
#endif

#define NORETURN      __attribute__((noreturn))
#define WEAK_SYMBOL   __attribute__((weak))
#ifdef __clang__
#define ALWAYS_INLINE_LAMBDA
#else
#define ALWAYS_INLINE_LAMBDA __attribute__((always_inline))
#endif
#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NOINLINE      __attribute__((noinline))

#define NON_COPYABLE(cls) \
    cls(const cls&) = delete; \
    cls& operator=(const cls&) = delete

#define NON_MOVEABLE(cls) \
    cls(cls&&) = delete; \
    cls& operator=(cls&&) = delete

namespace ams::util {

    /* Utilities for alignment to power of two. */
    template<typename T>
    constexpr ALWAYS_INLINE T AlignUp(T value, size_t alignment) {
        using U = typename std::make_unsigned<T>::type;
        const U invmask = static_cast<U>(alignment - 1);
        return static_cast<T>((value + invmask) & ~invmask);
    }

    template<typename T>
    constexpr ALWAYS_INLINE T AlignDown(T value, size_t alignment) {
        using U = typename std::make_unsigned<T>::type;
        const U invmask = static_cast<U>(alignment - 1);
        return static_cast<T>(value & ~invmask);
    }

    template<typename T>
    constexpr ALWAYS_INLINE bool IsAligned(T value, size_t alignment) {
        using U = typename std::make_unsigned<T>::type;
        const U invmask = static_cast<U>(alignment - 1);
        return (value & invmask) == 0;
    }

    template<typename T> requires std::unsigned_integral<T>
    constexpr ALWAYS_INLINE T GetAlignment(T value) {
        return value & -value;
    }

    template<>
    ALWAYS_INLINE void *AlignUp<void *>(void *value, size_t alignment) {
        return reinterpret_cast<void *>(AlignUp(reinterpret_cast<uintptr_t>(value), alignment));
    }

    template<>
    ALWAYS_INLINE const void *AlignUp<const void *>(const void *value, size_t alignment) {
        return reinterpret_cast<const void *>(AlignUp(reinterpret_cast<uintptr_t>(value), alignment));
    }

    template<>
    ALWAYS_INLINE void *AlignDown<void *>(void *value, size_t alignment) {
        return reinterpret_cast<void *>(AlignDown(reinterpret_cast<uintptr_t>(value), alignment));
    }

    template<>
    ALWAYS_INLINE const void *AlignDown<const void *>(const void *value, size_t alignment) {
        return reinterpret_cast<void *>(AlignDown(reinterpret_cast<uintptr_t>(value), alignment));
    }

    template<>
    ALWAYS_INLINE bool IsAligned<void *>(void *value, size_t alignment) {
        return IsAligned(reinterpret_cast<uintptr_t>(value), alignment);
    }

    template<>
    ALWAYS_INLINE bool IsAligned<const void *>(const void *value, size_t alignment) {
        return IsAligned(reinterpret_cast<uintptr_t>(value), alignment);
    }

}