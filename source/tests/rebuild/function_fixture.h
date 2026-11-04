#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>
#include "runtime/frontend/x86/decode.h"

namespace swift::tests::function_region {

// A real x86 loop of separated blocks, entered above its body. INT3 padding
// must stay unvisited. Every cycle adds blocks to EAX and decrements ECX once.
struct Fixture {
    static constexpr std::uint64_t base=0x1000;
    std::vector<std::uint8_t> bytes;
    unsigned blocks{},cycles{};
    std::uint64_t entry{},exit{};

    explicit Fixture(unsigned block_count,unsigned iterations=3)
        :blocks(block_count),cycles(iterations) {
        if (!blocks || blocks>2048 || !cycles) throw std::invalid_argument("invalid function fixture");
        const auto entry_offset=(blocks+1)*32;
        bytes.assign(entry_offset+15,0xcc);
        entry=base+entry_offset;exit=base+bytes.size()+0x10000;
        for (unsigned block=0;block<blocks;++block) {
            const auto at=block*32;
            bytes[at]=0x83;bytes[at+1]=0xc0;bytes[at+2]=1; // add eax,1
            if (block+1<blocks) Relative(at+3,0xe9,base+(block+1)*32);
            else {
                bytes[at+3]=0x83;bytes[at+4]=0xe9;bytes[at+5]=1; // sub ecx,1
                bytes[at+6]=0x0f;bytes[at+7]=0x85; // jne loop body
                Word(at+8,static_cast<std::uint32_t>(base-(base+at+12)));
                Relative(at+12,0xe9,exit);
            }
        }
        bytes[entry_offset]=0xb8;Word(entry_offset+1,0); // mov eax,0
        bytes[entry_offset+5]=0xb9;Word(entry_offset+6,cycles); // mov ecx,cycles
        Relative(entry_offset+10,0xe9,base);
    }
    x86::BlockWindow Window() const { return {base,bytes}; }
    x86::DecodeLimits Limits() const { return {blocks*2+5,blocks+2}; }

private:
    void Word(unsigned at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8));
    }
    void Relative(unsigned at,std::uint8_t opcode,std::uint64_t target) {
        bytes[at]=opcode;Word(at+1,static_cast<std::uint32_t>(target-(base+at+5)));
    }
};
} // namespace swift::tests::function_region
