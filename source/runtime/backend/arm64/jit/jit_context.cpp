//
// Created by 甘尧 on 2023/9/15.
//

#include "jit_context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"

namespace swift::runtime::backend::arm64 {

#define __ masm.

JitContext::JitContext(const std::shared_ptr<Module>& module, RegAlloc& reg_alloc)
        : module(module), reg_alloc(reg_alloc) {}

CPUReg JitContext::Get(const ir::Value& value) {
    if (reg_alloc.ValueType(value) == RegAlloc::GPR) {
        return X(value);
    } else if (reg_alloc.ValueType(value) == RegAlloc::FPR) {
        return V(value);
    } else if (reg_alloc.ValueType(value) == RegAlloc::MEM) {
        ASSERT_MSG(false, "");
    } else {
        ASSERT_MSG(false, "");
    }
}

Register JitContext::X(const ir::Value& value) {
    auto reg = reg_alloc.ValueGPR(value);
    return XRegister::GetXRegFromCode(reg.id);
}

VRegister JitContext::V(const ir::Value& value) {
    auto reg = reg_alloc.ValueFPR(value);
    return VRegister::GetVRegFromCode(reg.id);
}

Register JitContext::GetTmpX() {
    auto reg = reg_alloc.GetTmpGPR();
    return XRegister::GetXRegFromCode(reg.id);
}

VRegister JitContext::GetTmpV() {
    auto reg = reg_alloc.GetTmpFPR();
    return VRegister::GetVRegFromCode(reg.id);
}

void JitContext::Forward(ir::Location location) {
    ASSERT(cur_block);
    auto self_forward = location == cur_block->GetStartLocation();
    if (!self_forward && cur_function) {
        self_forward = location == cur_function->GetStartLocation();
    }
    if (self_forward) {
        auto self_label{GetLabel(location.Value())};
        __ B(self_label);
    } else {
        auto target_module = module->GetAddressSpace().GetModule(location.Value());
        if (!target_module) {
            // Module miss
            __ Mov(ipw, static_cast<u32>(HaltReason::ModuleMiss));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
            return;
        }

        const bool self_module_forward{module == target_module};
        const ModuleConfig& module_config{module->GetModuleConfig()};
        const ModuleConfig& target_module_config{target_module->GetModuleConfig()};

        bool direct_link{
                (self_module_forward && module_config.HasOpt(Optimizations::DirectBlockLink)) ||
                target_module_config.read_only};

        if (direct_link) {
            bool in_function{false};
            if (cur_function) {
                in_function = cur_function->FindBlock(location.Value());
            }
            if (in_function) {
                __ B(GetLabel(location.Value()));
            } else if (auto code = target_module->GetJitCache(location.Value()); code) {
                __ B(GetLabel(location.Value()));
            } else {
                BlockLinkStub(location);
            }
        } else if (self_module_forward && module_config.HasOpt(Optimizations::IndirectBlockLink)) {
            // indirect link
            u32 dispatcher_index = target_module->GetDispatchIndex(location);
            __ Mov(ipw, dispatcher_index);
            __ Ldr(ip, MemOperand(cache, ip, LSL, 3));
            __ Br(ip);
        } else {
            // do not link
            __ Mov(ip, location.Value());
            __ Str(ip, MemOperand(state, state_offset_current_loc));
            __ Ret();
        }
    }
}

void JitContext::ReturnToDispatcher(const vixl::aarch64::Register& location) {
    __ Str(location, MemOperand(state, state_offset_current_loc));
    __ Ret();
}

void JitContext::Finish() { __ FinalizeCode(); }

u8* JitContext::Flush(const CodeBuffer& code_cache) {
    FlushLabels(reinterpret_cast<VAddr>(code_cache.exec_data));
    Finish();
    std::memcpy(code_cache.rw_data, masm.GetBuffer()->GetStartAddress<u8*>(), code_cache.size);
    code_cache.Flush();
    return code_cache.exec_data;
}

u32 JitContext::CurrentBufferSize() { return __ GetBuffer() -> GetSizeInBytes(); }

void JitContext::SetCurrent(ir::Block* block) {
    cur_block = block;
    auto label = GetLabel(block->GetStartLocation().Value());
    __ Bind(label);
}

void JitContext::SetCurrent(ir::Function* function) {
    cur_function = function;
    auto label = GetLabel(function->GetStartLocation().Value());
    __ Bind(label);
}

void JitContext::TickIR(ir::Inst* instr) { reg_alloc.SetCurrent(instr); }

MacroAssembler& JitContext::GetMasm() { return masm; }

vixl::aarch64::Label* JitContext::GetLabel(LocationDescriptor location) {
    if (auto itr = labels.find(location); itr != labels.end()) {
        return &itr->second;
    } else {
        return &labels.try_emplace(location).first->second;
    }
}

void JitContext::BlockLinkStub(ir::Location location) {
    Label current;
    __ Bind(&current);
    __ Adr(ip0, &current);
    __ Str(ip0, MemOperand(state, state_offset_blocking_linkage_address));
    __ Mov(ipw0, (u32)HaltReason::BlockLinkage);
    __ Str(ipw0, MemOperand(state, state_offset_halt_reason));
    __ Mov(ip0, cur_block->GetStartLocation().Value());
    __ Str(ip0, MemOperand(state, state_offset_prev_loc));
    __ Mov(ip0, location.Value());
    __ Str(ip0, MemOperand(state, state_offset_current_loc));
    __ Ret();
}

void JitContext::FlushLabels(VAddr target) {
    for (auto& [location, label] : labels) {
        if (label.IsBound()) {
            continue;
        }
        ptrdiff_t offset = location - target;
        __ BindToOffset(&label, offset);
    }
}

#undef masm

}  // namespace swift::runtime::backend::arm64
