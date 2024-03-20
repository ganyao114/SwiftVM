//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <memory>
#include "config.h"

namespace swift::runtime {

using LocationDescriptor = size_t;

enum class HaltReason : std::uint32_t {
    None = 0x00000000,
    Step = 0x00000001,
    Signal = 0x00000002,
    PageFatal = 0x00000004,
    CodeMiss = 0x00000008,
    ModuleMiss = 0x000000010,
};

DECLARE_ENUM_FLAG_OPERATORS(HaltReason)

class Instance {};

class Interface {
public:
    explicit Interface(Config config);

    HaltReason Run();

    HaltReason Step();

    void SignalInterrupt();

    void ClearInterrupt();

    void SetLocation(LocationDescriptor location);

    LocationDescriptor GetLocation();

    [[nodiscard]] std::span<u8> GetUniformBuffer() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace swift::runtime