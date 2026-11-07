# The runner and offline compiler may have different link layouts. Persisted
# host references use semantic symbols; compatibility uses source/toolchain
# content, never executable addresses, mtimes or the checkout's absolute path.
file(GLOB_RECURSE svm_identity_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/source/base/*.h" "${PROJECT_SOURCE_DIR}/source/base/*.cpp"
    "${PROJECT_SOURCE_DIR}/source/aot/*.h" "${PROJECT_SOURCE_DIR}/source/aot/*.cpp"
    "${PROJECT_SOURCE_DIR}/source/runtime/*.h" "${PROJECT_SOURCE_DIR}/source/runtime/*.hpp"
    "${PROJECT_SOURCE_DIR}/source/runtime/*.inc" "${PROJECT_SOURCE_DIR}/source/runtime/*.cpp"
    "${PROJECT_SOURCE_DIR}/source/runtime/*.cc" "${PROJECT_SOURCE_DIR}/source/runtime/*.c"
    "${PROJECT_SOURCE_DIR}/source/runtime/*.S" "${PROJECT_SOURCE_DIR}/source/runtime/CMakeLists.txt")
list(APPEND svm_identity_sources "${PROJECT_SOURCE_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_FILE}")
list(SORT svm_identity_sources)
set(svm_identity "SwiftVM native-code ABI 1\n")
foreach(svm_source IN LISTS svm_identity_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${svm_source}")
    file(SHA256 "${svm_source}" svm_source_digest)
    file(RELATIVE_PATH svm_relative "${PROJECT_SOURCE_DIR}" "${svm_source}")
    string(APPEND svm_identity "${svm_relative}=${svm_source_digest}\n")
endforeach()
foreach(svm_variable IN ITEMS CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR CMAKE_SYSROOT
        CMAKE_C_COMPILER_ID CMAKE_C_COMPILER_VERSION CMAKE_C_COMPILER_TARGET
        CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION CMAKE_CXX_COMPILER_TARGET
        CMAKE_BUILD_TYPE CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_ASM_FLAGS
        CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE CMAKE_ASM_FLAGS_RELEASE
        CMAKE_C_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_ASM_FLAGS_RELWITHDEBINFO
        CMAKE_OSX_DEPLOYMENT_TARGET ANDROID_ABI ANDROID_PLATFORM SVM_ENABLE_IPO)
    string(APPEND svm_identity "${svm_variable}=${${svm_variable}}\n")
endforeach()
string(SHA256 svm_identity_digest "${svm_identity}")
file(CONFIGURE OUTPUT "${PROJECT_BINARY_DIR}/generated/svm/code_build_id.h"
    CONTENT "#pragma once\n#define SVM_CODE_BUILD_ID \"${svm_identity_digest}\"\n" @ONLY)
