//
// Created by 甘尧 on 2024/4/10.
//

#include "trampolines.h"

namespace swift::runtime::backend {

Trampolines::Trampolines(const Config& config, const CodeBuffer& buffer)
        : config(config), code_buffer(buffer) {}

bool Trampolines::LinkBlock(u8* source, u8* target, u8* source_rw, bool pic) { return false; }

}  // namespace swift::runtime::backend
