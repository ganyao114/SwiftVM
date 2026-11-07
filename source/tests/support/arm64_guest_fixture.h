#pragma once
#include <array>
#include <cstring>
#include "aarch64/assembler-aarch64.h"
#include "runtime/frontend/arm64/interpreter.h"
#include "tests/support/check.h"

namespace swift::tests::arm_guest {
namespace front = ::swift::arm64;
inline constexpr std::uint64_t CodeBase = 0x400000, DataBase = 0x500000;

struct Code {
    std::vector<std::uint32_t> words;
    std::size_t size{};

    template <class E> explicit Code(E emit) {
        vixl::aarch64::Assembler assembler(words);
        emit(assembler);
        size = assembler.GetSizeOfCodeGenerated();
    }

    std::span<const std::uint8_t> View() const {
        return {reinterpret_cast<const std::uint8_t*>(words.data()), size};
    }
};

struct Memory {
    const Code& code;
    std::array<std::uint8_t, 256> data{};
    unsigned reads{}, writes{};

    static front::CodeReadResult Fetch(void* opaque,
                                       std::uint64_t pc,
                                       std::span<std::uint8_t> out) noexcept {
        auto& self = *static_cast<Memory*>(opaque);
        if (pc < CodeBase || pc - CodeBase > self.code.size ||
            out.size() > self.code.size - (pc - CodeBase))
            return {{front::AccessStatus::Fault, pc}, 0};
        std::memcpy(out.data(), self.code.View().data() + pc - CodeBase, out.size());
        return {{}, out.size()};
    }

    static front::AccessResult Read(void* opaque,
                                    std::uint64_t address,
                                    std::span<std::uint8_t> out,
                                    bool) noexcept {
        auto& self = *static_cast<Memory*>(opaque);
        if (address < DataBase || address - DataBase > self.data.size() ||
            out.size() > self.data.size() - (address - DataBase))
            return {front::AccessStatus::Fault, address};
        std::memcpy(out.data(), self.data.data() + address - DataBase, out.size());
        ++self.reads;
        return {};
    }

    static front::AccessResult Write(void* opaque,
                                     std::uint64_t address,
                                     std::span<const std::uint8_t> out,
                                     bool) noexcept {
        auto& self = *static_cast<Memory*>(opaque);
        if (address < DataBase || address - DataBase > self.data.size() ||
            out.size() > self.data.size() - (address - DataBase))
            return {front::AccessStatus::Fault, address};
        std::memcpy(self.data.data() + address - DataBase, out.data(), out.size());
        ++self.writes;
        return {};
    }

    front::DirectServices Services() { return {this, Fetch, Read, Write, nullptr}; }
};

inline front::StepResult Run(runtime::arm64::GuestState& state, Memory& memory) {
    front::StepResult result;
    for (unsigned i = 0; i < 64; ++i) {
        result = front::InterpretOne(state, memory.Services());
        if (result.kind != front::StepKind::Completed) return result;
    }
    Check(false, "ARM guest test exceeded its bounded instruction count");
    return result;
}
}  // namespace swift::tests::arm_guest
