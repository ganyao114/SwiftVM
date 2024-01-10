add_library(fronted_arm64 STATIC
        arm64_frontend.cpp
        arm64_frontend.h)

target_link_libraries(fronted_arm64 PUBLIC swift_runtime vixl)