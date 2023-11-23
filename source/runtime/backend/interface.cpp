//
// Created by 甘尧 on 2023/9/7.
//
#pragma once

#include "runtime/common/types.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/function.h"
#include "runtime/backend/translate_table.h"
#include "runtime/backend/context.h"
#include "runtime/backend/address_space.h"

namespace swift::runtime {

constexpr static auto l1_cache_bits = 18;

struct Interface::Impl final {
    Impl(Interface* interface, Config conf) : interface(interface), conf(std::move(conf)) {}

    backend::State* state;
    Interface* interface;
    Config conf;
    ir::BlockMap *ir_blocks{};
    ir::FunctionMap *ir_functions{};
    TranslateTable l1_code_cache{l1_cache_bits};
};

Interface::Interface(Config config) : impl(std::make_unique<Impl>(this, std::move(config))) {

}

u32 Interface::Run() {
    return 0;
}

u32 Interface::Step() {
    return 0;
}

void Interface::SetLocation(LocationDescriptor location) {
    impl->state->current_loc = ir::Location(location);
}

LocationDescriptor Interface::GetLocation() {
    return impl->state->current_loc.Value();
}

}  // namespace swift::runtime