//
// Created by 甘尧 on 2024/6/22.
//

#include <span>
#include "base/types.h"

namespace swift::linux {

int Execve(const char *program, std::span<char *> args, std::span<char *> envps);

}
