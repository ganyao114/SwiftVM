//
// Created by SwiftGan on 2021/1/2.
//

#pragma once

#include "translator/x86/cpu.h"
#include "runtime/frontend/function_abi.h"

namespace swift::x86 {

runtime::frontend::ABIDescriptor GetABIDescriptor32();
runtime::frontend::ABIDescriptor GetABIDescriptor64();

}