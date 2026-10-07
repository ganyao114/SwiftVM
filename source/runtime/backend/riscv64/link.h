#pragma once
#include "runtime/backend/code_serial.h"
#include "runtime/backend/context.h"
#include "runtime/backend/module.h"

namespace swift::runtime::backend::riscv64 {
void* ResolveDirectLink(State* state, const u8* site) noexcept;
bool RegisterLinkSites(const std::shared_ptr<Module>& module, const CodeBuffer& buffer,
                       std::span<const SerialLinkSite> sites);
void RecordCode(const std::shared_ptr<Module>& module, const CodeBuffer& buffer,
                u64 guest, bool function, std::vector<SerialBlock> blocks,
                std::span<const SerialLinkSite> links);
} // namespace swift::runtime::backend::riscv64
