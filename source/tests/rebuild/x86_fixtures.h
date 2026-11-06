#pragma once

#include <array>
#include <cstdint>
#include "runtime/frontend/x86/build_ir.h"

namespace swift::tests::x86_ir {

// Real x86-64 bytes; these are decoded, never executed on the host/device.
inline constexpr auto entry = std::to_array<std::uint8_t>({
        0x48, 0xb8, 0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12,  // movabs rax, imm64
        0xb9, 0xff, 0xff, 0xff, 0xff,                                // mov ecx, -1
        0x48, 0x89, 0xc2,                                            // mov rdx, rax
        0x48, 0x8d, 0x54, 0x88, 0xf0,                                // lea rdx, [rax+rcx*4-16]
        0x48, 0x89, 0x11,                                            // mov [rcx], rdx
        0xeb, 0x24,                                                  // jmp 0x1040
});
inline constexpr auto middle = std::to_array<std::uint8_t>({
        0x48, 0x8b, 0x05, 0x10, 0x00, 0x00, 0x00,  // mov rax, [rip+16]
        0x89, 0xd8,                                // mov eax, ebx
        0x48, 0xc7, 0x01, 0xff, 0xff, 0xff, 0xff,  // mov qword [rcx], -1
        0xe9, 0x2b, 0x00, 0x00, 0x00,              // jmp 0x1080
});
inline constexpr auto return_body = std::to_array<std::uint8_t>({
        0x90,  // nop
        0x48,
        0x8b,
        0x04,
        0xc8,  // mov rax, [rax+rcx*8]
        0xc3,  // ret
});
inline constexpr x86::StateLayout layout{0x20, 0x200, 8};
inline constexpr std::array<x86::BlockWindow, 3> windows{{
        {0x1000, entry},
        {0x1040, middle},
        {0x1080, return_body},
}};

}  // namespace swift::tests::x86_ir
