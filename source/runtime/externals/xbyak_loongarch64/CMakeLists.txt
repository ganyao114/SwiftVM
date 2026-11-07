# Vendored from loongson/xbyak_loongarch64 at
# 21af45e2ae09151804c71c6931d50fb212f35f93 (Apache-2.0).
add_library(swift_xbyak_loongarch64 STATIC src/xbyak_loongarch64_impl.cpp)
target_include_directories(swift_xbyak_loongarch64 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/xbyak_loongarch64)
target_compile_features(swift_xbyak_loongarch64 PUBLIC cxx_std_20)
set_target_properties(swift_xbyak_loongarch64 PROPERTIES POSITION_INDEPENDENT_CODE ON)
