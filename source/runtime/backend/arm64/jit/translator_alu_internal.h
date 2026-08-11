#pragma once

namespace swift::runtime::backend::arm64 {

VRegister VecLaneFormat(VRegister reg, u32 lane_bits);

}  // namespace swift::runtime::backend::arm64
