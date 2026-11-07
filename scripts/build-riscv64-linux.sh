#!/bin/sh
set -eu

task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build=${1:-"$task_root/build-riscv64-linux"}
if [ "$#" -gt 0 ]; then shift; fi

cmake -S "$task_root" -B "$task_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$task_root/source/cmake/riscv64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$task_build" \
    --target svm_rebuild_linux svm_riscv64_compile_test svm_runtime_engine_test \
    --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
