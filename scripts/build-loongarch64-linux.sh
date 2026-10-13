#!/bin/sh
set -eu

task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build=${1:-"$task_root/build-loongarch64-linux"}
if [ "$#" -gt 0 ]; then shift; fi
task_jobs=${SVM_BUILD_JOBS:-3}
cmake -S "$task_root" -B "$task_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$task_root/source/cmake/loongarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$task_build" --target svm_translator_linux swift_loongarch_backend_test \
    swift_loongarch_assembler_test swift_loongarch_assembler_range_test \
    swift_loongarch_lbt_encoding_test \
    --parallel "$task_jobs"
