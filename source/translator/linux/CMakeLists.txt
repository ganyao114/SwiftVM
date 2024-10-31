add_executable(svm_translator_linux
        loader.cpp
        loader.h
        main.cpp
        linker/linker_bridge.cpp
        linker/linker_bridge.h
        linker/ldso/dynlink.c
        linker/ldso/dynlibc.h
        linker/ldso/dynconfig.h
        linker/linker.cpp
        linker/linker.h)

target_link_libraries(svm_translator_linux PUBLIC swift_translator)