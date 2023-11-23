//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <memory>
#include "config.h"

namespace swift::runtime {

using LocationDescriptor = size_t;

class Instance {};

class Interface {
public:
    explicit Interface(Config config);

    u32 Run();

    u32 Step();

    void SetLocation(LocationDescriptor location);

    LocationDescriptor GetLocation();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace swift::runtime