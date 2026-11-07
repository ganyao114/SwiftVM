#pragma once

#include <string>
#include <string_view>

#include "types.h"

namespace swift::runtime::ir {
class Program;

// Optional diagnostic output. The sink consumes each borrowed text chunk
// before returning; neither the formatter nor Program retains it.
struct FormatSink {
    void* context{};
    void (*write)(void*, std::string_view){};
};

void FormatTo(FormatSink, const Program&);
void FormatOperationTo(FormatSink, const Program&, NodeId);
std::string ToString(const Program&);
}  // namespace swift::runtime::ir
