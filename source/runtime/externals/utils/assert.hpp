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

#include "common.hpp"
#include "assert.h"

#define AMS_ASSERT_IMPL(type, expr, ...) assert(expr)
#define AMS_ASSERT(expr, ...) assert(expr)

#if defined(__cplusplus)

namespace ams::impl {

    template<typename... ArgTypes>
    constexpr ALWAYS_INLINE void UnusedImpl(ArgTypes &&... args) {
        (static_cast<void>(args), ...);
    }

}

#endif

#define AMS_UNUSED(...) ::ams::impl::UnusedImpl(__VA_ARGS__)