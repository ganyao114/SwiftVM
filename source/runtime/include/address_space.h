//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <memory>

namespace swift::runtime {

class AddressSpace {
public:
    static std::unique_ptr<AddressSpace> Make();

    virtual void InvalidateAllCache() = 0;
};

}
