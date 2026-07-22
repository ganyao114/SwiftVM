# SwiftVM 动态链接器构建配置
cmake_minimum_required(VERSION 3.15)

# 动态链接器核心
add_library(swiftvm_dynlink STATIC
    dynlink.c
    linker_bridge.cpp
    linker.cpp
)

target_include_directories(swiftvm_dynlink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/source/base
    ${CMAKE_SOURCE_DIR}/source/runtime/include
)

target_compile_definitions(swiftvm_dynlink PRIVATE
    _GNU_SOURCE
    SWIFTVM_GUEST_SUPPORT
)

# 根据架构添加桥接生成器
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_definitions(swiftvm_dynlink PRIVATE SWIFTVM_HOST_X86_64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_definitions(swiftvm_dynlink PRIVATE SWIFTVM_HOST_ARM64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "riscv64")
    target_compile_definitions(swiftvm_dynlink PRIVATE SWIFTVM_HOST_RISCV64)
endif()

# 链接依赖
target_link_libraries(swiftvm_dynlink
    PRIVATE
        ${CMAKE_DL_LIBS}  # libdl
        pthread
)

# 编译选项
target_compile_options(swiftvm_dynlink PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -fPIC
)

# C++ 标准
set_target_properties(swiftvm_dynlink PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# 可选：构建独立的动态链接器可执行文件（类似 ld-linux.so）
option(BUILD_STANDALONE_LINKER "Build standalone dynamic linker executable" ON)

if(BUILD_STANDALONE_LINKER)
    add_executable(swiftvm-ld
        dynlink.c
        linker_bridge.cpp
        linker.cpp
    )
    
    target_include_directories(swiftvm-ld PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/source/base
        ${CMAKE_SOURCE_DIR}/source/runtime/include
    )
    
    target_compile_definitions(swiftvm-ld PRIVATE
        _GNU_SOURCE
        SWIFTVM_GUEST_SUPPORT
        SWIFTVM_STANDALONE_LINKER
    )
    
    target_link_libraries(swiftvm-ld
        PRIVATE
            ${CMAKE_DL_LIBS}
            pthread
    )
    
    target_compile_options(swiftvm-ld PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
    
    set_target_properties(swiftvm-ld PROPERTIES
        CXX_STANDARD 17
        C_STANDARD 11
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    
    # 安装到系统
    install(TARGETS swiftvm-ld
        RUNTIME DESTINATION bin
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                    GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE
    )
endif()

# 测试程序（可选）
option(BUILD_LINKER_TESTS "Build dynamic linker tests" OFF)

if(BUILD_LINKER_TESTS)
    add_subdirectory(tests)
endif()

# 安装头文件
install(FILES
    linker_bridge.h
    linker.h
    DESTINATION include/swiftvm/linker
)

# 安装库文件
install(TARGETS swiftvm_dynlink
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

# 生成配置文件
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/linker_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/linker_config.h
    @ONLY
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/linker_config.h
    DESTINATION include/swiftvm/linker
)