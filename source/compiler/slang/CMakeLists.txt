antlr4_gen(${CMAKE_CURRENT_SOURCE_DIR}/antlr ${CMAKE_CURRENT_BINARY_DIR}/antlr)
file(GLOB G4_DEFS ${CMAKE_CURRENT_BINARY_DIR}/antlr/*.h)
file(GLOB G4_SRCS ${CMAKE_CURRENT_BINARY_DIR}/antlr/*.cpp)

add_library(slang_compiler STATIC
        ${G4_SRCS}
        ${G4_DEFS}
        slang.cpp
        slang.h
        context.cpp
        context.h
        class.cpp
        class.h
        method.cpp
        method.h
        expression.cpp
        expression.h
        attrs.h
        field.cpp
        field.h)

target_link_libraries(slang_compiler PUBLIC swift::base antlr4_static)
target_include_directories(slang_compiler PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/antlr)