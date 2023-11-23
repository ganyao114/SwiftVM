add_library(dlmalloc STATIC
        dlmalloc/malloc.h
        dlmalloc/malloc.c)

target_include_directories(dlmalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})