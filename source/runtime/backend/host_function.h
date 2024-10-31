//
// Created by 甘尧 on 2024/7/22.
//

#pragma once

#include <vector>
#include "base/common_funcs.h"
#include "runtime/common/cityhash.h"
#include "runtime/include/config.h"

namespace swift::runtime::backend {

enum class ParamType : u32 {
    Void = 0,
    Point,
    Uint8,
    Uint16,
    Uint32,
    Uint64,
    Int8,
    Int16,
    Int32,
    Int64,
    Float8,
    Float16,
    Float32,
    Float64,
    Float128,
    Struct
};

DECLARE_ENUM_FLAG_OPERATORS(ParamType)

constexpr u32 GetStructSize(ParamType type) {
    return (u32) type - (u32) ParamType::Struct;
}

struct HostFunction {
    std::string module;
    std::string name;
    // return type = signatures[0]
    std::vector<ParamType> signatures;
    LocationDescriptor addr;
    void* impl;

    [[nodiscard]] u64 SignatureHash() {
        return CityHash64(reinterpret_cast<const char*>(signatures.data()),
                          signatures.size() * sizeof(ParamType));
    }
};

}
