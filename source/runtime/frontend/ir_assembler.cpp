//
// Created by 甘尧 on 2023/12/19.
//

#include "ir_assembler.h"

namespace swift::runtime::ir {

HIRBuilder::ElseThen Assembler::If(const terminal::If& if_) {
    if (hir_builder) {
        return hir_builder->If(if_);
    } else {
        end_decode = true;
        return {};
    }
}

HIRBlock* Assembler::LinkBlock(const terminal::LinkBlock& block) {
    if (hir_builder) {
        return hir_builder->LinkBlock(block);
    } else {
        end_decode = true;
        return {};
    }
}

void Assembler::ReturnToDispatcher() {
    if (hir_builder) {
        hir_builder->ReturnToDispatcher();
    } else {
        end_decode = true;
    }
}

void Assembler::ReturnToHost() {
    end_decode = true;
    if (hir_builder) {
        hir_builder->ReturnToHost();
    } else {
    }
}

void Assembler::Return() {
    end_decode = true;
    if (hir_builder) {
        hir_builder->Return();
    } else {

    }
}

bool Assembler::EndCommit() const {
    return end_decode;
}

}
