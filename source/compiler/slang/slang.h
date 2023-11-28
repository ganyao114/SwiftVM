//
// Created by 甘尧 on 2023/11/24.
//

#pragma once

#include <string>
#include "compiler/slang/context.h"

namespace swift::slang {

void CompileFile(const std::string &path, Context &context);
void CompileContent(const std::string &content, Context &context);

}
