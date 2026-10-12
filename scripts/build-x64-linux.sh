#!/bin/sh
set -eu

task_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
task_build=${1:-"$task_root/build/x64-linux"}
if [ "$#" -gt 0 ]; then shift; fi
cmake -S "$task_root" -B "$task_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build "$task_build" --target svm_translator_linux \
    swift_x64_backend_test swift_x64_phi_test swift_exclusive_monitor_test \
    swift_memory_lease_test \
    arm64_frontend_semantics_test --parallel "${SVM_BUILD_JOBS:-2}"
ctest --test-dir "$task_build" \
    -R '^(swift_x64_backend_test|swift_x64_phi_test|swift_exclusive_monitor_test|swift_memory_lease_test|arm64_frontend_semantics_test)$' \
    --no-tests=error --output-on-failure
