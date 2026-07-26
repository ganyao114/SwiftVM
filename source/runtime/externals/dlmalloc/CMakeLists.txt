add_library(dlmalloc STATIC
        dlmalloc/malloc.h
        dlmalloc/malloc.c)

target_include_directories(dlmalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# SwiftVM uses dlmalloc only for private, caller-supplied mspaces (the JIT
# code cache and module arenas). Do not export its process-wide malloc/free:
# on macOS, libc APIs can return allocations from a libSystem malloc zone to
# executable code, whose interposed dlmalloc free would reject/corrupt them.
target_compile_definitions(dlmalloc PRIVATE ONLY_MSPACES=1)
