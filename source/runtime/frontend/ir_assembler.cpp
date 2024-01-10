//
// Created by 甘尧 on 2023/12/19.
//

#include "ir_assembler.h"

namespace swift::runtime::ir {

HIRBuilder::ElseThen Assembler::If(const terminal::If& if_) {
    return hir_builder->If(if_);
}

HIRBlock* Assembler::LinkBlock(const terminal::LinkBlock& block) {
    return hir_builder->LinkBlock(block);
}

void Assembler::ReturnToDispatcher() {
    hir_builder->ReturnToDispatcher();
}

void Assembler::ReturnToHost() {
    hir_builder->ReturnToHost();
}

void Assembler::Return() {
    hir_builder->Return();
}

}
