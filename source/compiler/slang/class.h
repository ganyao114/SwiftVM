//
// Created by 甘尧 on 2023/11/28.
//

#pragma once

#include <unordered_map>
#include <string>
#include "compiler/slang/method.h"

namespace swift::slang {

class Class {
public:

private:
    std::string name_space;
    std::string name;
    std::unordered_map<std::string, Method> methods;
    Method *cur_method;
};

}