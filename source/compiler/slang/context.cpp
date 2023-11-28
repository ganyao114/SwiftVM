//
// Created by 甘尧 on 2023/11/28.
//

#include "context.h"

namespace swift::slang {

void Context::SetNamespace(const std::string& name) {
    cur_namespace = name;
}

}