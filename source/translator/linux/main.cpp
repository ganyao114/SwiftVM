//
// Created by swift on 6/27/24.
//

#include "loader.h"
#include "translator/x86/translator.h"

int main_(int argc, char **argv) {
    swift::linux::Execve("/Users/swift/CLionProjects/SwiftVM/source/translator/linux/tests/test", {}, {});
    return 0;
}
