//
// Created by 甘尧 on 2023/11/28.
//

#pragma once

#include <string>
#include "attrs.h"
#include "compiler/slang/expression.h"

namespace swift::slang {

class Field {
private:
    std::string name;
    AccessFlag access_flag;
};

}


