//
// Created by 甘尧 on 2024/7/18.
//

#include "linker.h"

#include <utility>

namespace swift::linux {

ShadowLib::ShadowLib(std::string path) : path(std::move(path)) {}

}
