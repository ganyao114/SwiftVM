//
// Created by 甘尧 on 2023/11/28.
//

#pragma once

namespace swift::slang {

enum class AccessFlag {
    Private,
    Protect,
    Public
};

enum class Type {
    Void,
    Bool,
    S32,
    S64,
    U32,
    U64,
    String,
    Object
};

}
