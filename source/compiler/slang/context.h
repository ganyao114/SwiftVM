//
// Created by 甘尧 on 2023/11/28.
//

#pragma once

#include <string>
#include <unordered_map>
#include "compiler/slang/class.h"

namespace swift::slang {

class Context {
public:

    void SetNamespace(const std::string &name);

private:
    std::unordered_map<std::string, Class> classes;
    std::string cur_namespace;
    Class *cur_class;
};

}
