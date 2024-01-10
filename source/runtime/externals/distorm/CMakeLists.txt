add_library(distorm STATIC
        decoder.c
        decoder.h
        distorm.c
        distorm.h
        instructions.c
        instructions.h
        insts.c
        insts.h
        mnemonics.c
        mnemonics.h
        operands.c
        operands.h
        prefix.c
        prefix.h
        textdefs.c
        textdefs.h
        wstring.c
        wstring.h)

target_include_directories(distorm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
