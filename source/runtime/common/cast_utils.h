//
// Created by 甘尧 on 2024/1/8.
//

#pragma once

#include "function_info.h"

namespace swift::runtime {

/// Cast a lambda into an equivalent function pointer.
template<class Function>
inline auto FptrCast(Function f) noexcept {
    return static_cast<equivalent_function_type<Function>*>(f);
}

template<class Return_Type, class Argument_Type>
inline Return_Type &ForceCast(Argument_Type &rSrc) {
    return(*reinterpret_cast<Return_Type *>(&rSrc));
}

}