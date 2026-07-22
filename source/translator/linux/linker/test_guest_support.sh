#!/bin/bash
# SwiftVM Guest 程序测试脚本

set -e

# 配置
SWIFTVM_ROOT="/path/to/swiftvm"
GUEST_PROGRAM="./test_guest_program"
HOST_LIBS="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu"
GUEST_LIBS="/guest/lib:/guest/usr/lib"

# 函数：检查 host 库
check_host_libs() {
    echo "检查 host 系统库..."
    
    local libs=("libc.so.6" "libpthread.so.0" "libm.so.6" "libdl.so.2")
    for lib in "${libs[@]}"; do
        if ldconfig -p | grep -q "$lib"; then
            echo "✓ $lib 可用"
        else
            echo "✗ $lib 不可用"
            return 1
        fi
    done
}

# 函数：测试基本功能
test_basic_execution() {
    echo "测试基本程序执行..."
    
    # 创建简单的测试程序
    cat > test_program.c << 'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

void* thread_func(void* arg) {
    printf("Thread: Hello from %s\n", (char*)arg);
    return NULL;
}

int main() {
    printf("SwiftVM Guest Program Test\n");
    printf("Math test: sqrt(16) = %f\n", sqrt(16.0));
    
    // 测试动态内存分配
    char* buffer = malloc(100);
    strcpy(buffer, "Dynamic allocation works!");
    printf("Memory test: %s\n", buffer);
    free(buffer);
    
    // 测试线程
    pthread_t thread;
    char* message = "guest thread";
    pthread_create(&thread, NULL, thread_func, message);
    pthread_join(thread, NULL);
    
    printf("All tests passed!\n");
    return 0;
}
EOF

    # 编译为 guest 程序
    gcc -o test_guest_program test_program.c -lm -lpthread
    
    echo "编译完成：test_guest_program"
}

# 函数：测试库依赖查看
test_library_dependencies() {
    echo "测试库依赖查看..."
    
    if [ -f "$SWIFTVM_ROOT/ld.so" ]; then
        echo "使用 SwiftVM 动态链接器查看依赖："
        $SWIFTVM_ROOT/ld.so --list ./test_guest_program
    else
        echo "使用系统 ldd 查看依赖："
        ldd ./test_guest_program
    fi
}

# 函数：测试环境变量
test_environment_variables() {
    echo "测试环境变量支持..."
    
    # 设置库路径
    export LD_LIBRARY_PATH="$GUEST_LIBS:$LD_LIBRARY_PATH"
    
    # 测试预加载（如果有 guest 特定库）
    if [ -f "/guest/lib/guest_interceptor.so" ]; then
        export LD_PRELOAD="guest_interceptor.so"
        echo "设置了 LD_PRELOAD: $LD_PRELOAD"
    fi
    
    echo "设置了 LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
}

# 函数：性能测试
test_performance() {
    echo "运行性能测试..."
    
    cat > perf_test.c << 'EOF'
#include <stdio.h>
#include <time.h>
#include <math.h>

#define ITERATIONS 1000000

int main() {
    clock_t start, end;
    double cpu_time_used;
    
    printf("性能测试开始...\n");
    
    start = clock();
    for (int i = 0; i < ITERATIONS; i++) {
        sqrt(i * 3.14159);
    }
    end = clock();
    
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("数学运算测试：%f 秒 (%d iterations)\n", cpu_time_used, ITERATIONS);
    
    start = clock();
    for (int i = 0; i < ITERATIONS; i++) {
        char* ptr = malloc(64);
        free(ptr);
    }
    end = clock();
    
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;
    printf("内存分配测试：%f 秒 (%d iterations)\n", cpu_time_used, ITERATIONS);
    
    return 0;
}
EOF

    gcc -o perf_test perf_test.c -lm
    ./perf_test
}

# 函数：调试模式测试
test_debug_mode() {
    echo "测试调试模式..."
    
    # 如果 SwiftVM 支持调试输出
    if [ -f "$SWIFTVM_ROOT/ld.so" ]; then
        echo "启用详细调试输出："
        LD_DEBUG=all $SWIFTVM_ROOT/ld.so ./test_guest_program 2>&1 | head -20
    fi
}

# 函数：错误处理测试
test_error_handling() {
    echo "测试错误处理..."
    
    # 创建故意缺少依赖的程序
    cat > error_test.c << 'EOF'
#include <stdio.h>

// 声明一个不存在的函数
extern void nonexistent_function(void);

int main() {
    printf("这个程序应该会报错...\n");
    nonexistent_function();  // 这会导致链接错误
    return 0;
}
EOF

    echo "编译故意有错误的程序..."
    if gcc -o error_test error_test.c 2>/dev/null; then
        echo "意外：程序编译成功了"
    else
        echo "✓ 编译时正确检测到错误"
    fi
}

# 主测试流程
main() {
    echo "SwiftVM Guest 程序支持测试"
    echo "================================"
    
    # 检查环境
    check_host_libs
    
    # 运行测试
    test_basic_execution
    test_library_dependencies
    test_environment_variables
    
    # 执行程序
    echo "执行 guest 程序..."
    if [ -f "$SWIFTVM_ROOT/ld.so" ]; then
        echo "使用 SwiftVM 动态链接器："
        $SWIFTVM_ROOT/ld.so ./test_guest_program
    else
        echo "直接执行："
        ./test_guest_program
    fi
    
    # 性能和调试测试
    test_performance
    test_debug_mode
    test_error_handling
    
    # 清理
    echo "清理测试文件..."
    rm -f test_program.c test_guest_program perf_test.c perf_test error_test.c error_test
    
    echo "================================"
    echo "测试完成！"
}

# 如果直接运行此脚本
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi
