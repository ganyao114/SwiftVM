add_library(fronted_arm64 STATIC
        arm64_frontend.cpp
        arm64_frontend_branch.cpp
        arm64_frontend_dataproc.cpp
        arm64_frontend_internal.h
        arm64_frontend_ls.cpp
        arm64_frontend_neon.cpp
        arm64_frontend.h)

target_link_libraries(fronted_arm64 PUBLIC swift_runtime vixl)

add_executable(arm64_decode_smoke arm64_decode_smoke.cpp)
target_link_libraries(arm64_decode_smoke PRIVATE fronted_arm64)
