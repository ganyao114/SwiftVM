//
// Shared index of the guest ABI stubs.  The guest program (abi_stubs.c) emits
// a table of function pointers at its ELF entry point in exactly this order;
// the host test reads entry+8*i to get the guest address of stub i.
//
// This is how the host finds guest functions in a binary that mklinuxelf.py
// produces without a section/symbol table (see docs/aot-design.md §9).
//
#ifndef SVM_ABI_STUBS_INDEX_H
#define SVM_ABI_STUBS_INDEX_H

enum SvmStubIndex {
    kStubIntSum9 = 0,       // long(long x9)                  6 regs + 3 stack
    kStubDblSum10,          // double(double x10)             8 xmm  + 2 stack
    kStubMix8,              // double(long,double, ... x4)    independent counters
    kStubTwoInt,            // long(struct TwoInt)            1 INTEGER eightbyte
    kStubTwoDouble,         // double(struct TwoDouble)       SSE, SSE
    kStubDoubleLong,        // double(struct DoubleLong)      SSE, INTEGER
    kStubLongDouble,        // double(struct LongDouble)      INTEGER, SSE
    kStubTwoFloat,          // double(struct TwoFloat)        one SSE eightbyte
    kStubIntFloat,          // double(struct IntFloat)        one INTEGER eightbyte
    kStubChars8,            // long(struct Chars8)            one INTEGER eightbyte
    kStubMakeBig,           // struct Big24(long,long,long)   MEMORY return
    kStubBigIdent,          // struct Big24(struct Big24,long) MEMORY in and out
    kStubTakeBig,           // long(struct Big24, long)       MEMORY arg
    kStubRetTwoDouble,      // struct TwoDouble(double,double) xmm0/xmm1 return
    kStubRetDoubleLong,     // struct DoubleLong(double,long)  xmm0/rax return
    kStubRetTwoInt,         // struct TwoInt(int,int)          rax return
    kStubVarargInts,        // long(int n, ...)   integer varargs
    kStubVarargDoubles,     // double(int n, ...) double varargs -> needs %al
    kStubVarargMixed,       // double(int n, ...) alternating long/double
    kStubGetRsp,            // long(void)  returns %rsp on entry
    kStubGetAl,             // long(void)  returns %al on entry
    kStubDumpArgs,          // void(...)   dumps the whole argument register file
    kStubCrash,             // long(void)  wild store -> guest fault
    kStubUd2,               // long(void)  illegal instruction
    kStubUnbalanced,        // long(void)  returns with a corrupted %rsp
    kStubStructBySpill,     // double(double x8, struct TwoDouble) spilled struct
    kStubIntFloatMix,       // long(...) 6 ints then a float; SSE still available
    kStubI128AfterStack,    // long(long x7, __int128) 16-aligned stack slot
    kStubRetTwoLong,        // struct TwoLong(long,long)  INTEGER,INTEGER -> rax,rdx
    kStubRunaway,           // long(void)  unbounded recursion -> stack exhaustion
    kStubCount
};

// Absolute guest address of the argument-dump buffer written by kStubDumpArgs.
// A fixed address, because the stub ELF has no symbol table for the host to
// look one up in.
#define SVM_DUMP_ADDR 0x30000000UL

// Layout of that buffer.
#define SVM_DUMP_OFF_RDI 0x00
#define SVM_DUMP_OFF_RSI 0x08
#define SVM_DUMP_OFF_RDX 0x10
#define SVM_DUMP_OFF_RCX 0x18
#define SVM_DUMP_OFF_R8 0x20
#define SVM_DUMP_OFF_R9 0x28
#define SVM_DUMP_OFF_RAX 0x30
#define SVM_DUMP_OFF_RSP 0x38
#define SVM_DUMP_OFF_XMM 0x40  /* 8 * 16 bytes */
#define SVM_DUMP_OFF_STACK 0xC0 /* 16 qwords starting at [rsp+8] */
#define SVM_DUMP_SIZE 0x140

#endif
