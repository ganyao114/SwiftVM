antlr4_gen(${CMAKE_CURRENT_SOURCE_DIR}/antlr ${CMAKE_CURRENT_BINARY_DIR}/antlr)
file(GLOB G4_DEFS ${CMAKE_CURRENT_BINARY_DIR}/antlr/*.h)
file(GLOB G4_SRCS ${CMAKE_CURRENT_BINARY_DIR}/antlr/*.cpp)

add_library(clang_compiler STATIC
        ${G4_SRCS}
        ${G4_DEFS}
        clang.cpp
        clang.h)

target_link_libraries(clang_compiler PUBLIC swift::base antlr4_static)
target_include_directories(clang_compiler PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/antlr)