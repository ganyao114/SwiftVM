//
// Smoke test for the AArch64 frontend: hand-assembles a few guest blocks with
// the VIXL assembler, decodes them with A64Decoder and prints the IR.
//

#include <cstring>
#include <vector>
#include "aarch64/macro-assembler-aarch64.h"
#include "arm64_frontend.h"
#include "fmt/format.h"
#include "runtime/ir/block.h"

using namespace swift;
using namespace swift::arm64;
using namespace swift::runtime;

namespace {

constexpr VAddr kGuestBase = 0x400000;

// Flat guest memory backed by a host buffer, identity mapped at kGuestBase.
class FlatMemory final : public MemoryInterface {
public:
    explicit FlatMemory(size_t size) : buffer_(size, 0) {}

    bool Read(void* dest, size_t addr, size_t size) override {
        if (addr < kGuestBase || addr + size > kGuestBase + buffer_.size()) return false;
        std::memcpy(dest, buffer_.data() + (addr - kGuestBase), size);
        return true;
    }

    bool Write(void* src, size_t addr, size_t size) override {
        if (addr < kGuestBase || addr + size > kGuestBase + buffer_.size()) return false;
        std::memcpy(buffer_.data() + (addr - kGuestBase), src, size);
        return true;
    }

    void* GetPointer(void* src) override {
        auto addr = reinterpret_cast<size_t>(src);
        if (addr < kGuestBase || addr >= kGuestBase + buffer_.size()) return nullptr;
        return buffer_.data() + (addr - kGuestBase);
    }

    u8* Data() { return buffer_.data(); }

private:
    std::vector<u8> buffer_;
};

template <typename EmitFn>
void BuildGuestCode(FlatMemory& memory, EmitFn&& emit) {
    vixl::aarch64::MacroAssembler masm(memory.Data(), 4096);
    emit(masm);
    masm.FinalizeCode();
}

void DecodeAndPrint(FlatMemory& memory, VAddr start, const char* name) {
    auto block = new ir::Block(0, start);
    ir::Assembler assembler{block};
    A64Decoder decoder{start, &memory, &assembler};
    decoder.Decode();

    fmt::print("=== block {} @ {:#x} ===\n", name, start);
    for (auto& inst : block->GetInstList()) {
        fmt::print("  {}\n", inst);
    }
    delete block;
}

}  // namespace

int main() {
    FlatMemory memory(4096);

    // Block 1: syscall sequence (write(1, msg, 12); exit style prologue).
    BuildGuestCode(memory, [](vixl::aarch64::MacroAssembler& masm) {
        using namespace vixl::aarch64;
        Label msg;
        masm.Mov(x0, 1);
        masm.Adr(x1, &msg);
        masm.Mov(x2, 12);
        masm.Mov(x8, 64);
        masm.Svc(0);
        masm.Bind(&msg);
    });
    DecodeAndPrint(memory, kGuestBase, "svc");

    // Block 2: cmp + conditional branch + add/sub shifted/logical.
    BuildGuestCode(memory, [](vixl::aarch64::MacroAssembler& masm) {
        using namespace vixl::aarch64;
        Label done;
        masm.Cmp(w0, 0);
        masm.B(&done, eq);
        masm.Add(w0, w0, Operand(w1, LSL, 2));
        masm.Orr(w0, w0, 0xFF);
        masm.Bind(&done);
        masm.Ret();
    });
    DecodeAndPrint(memory, kGuestBase, "cond_branch");

    // Block 3: function prologue / epilogue (stp pre-index, ldp post-index).
    BuildGuestCode(memory, [](vixl::aarch64::MacroAssembler& masm) {
        using namespace vixl::aarch64;
        masm.Stp(x29, x30, MemOperand(sp, -16, PreIndex));
        masm.Mov(x29, sp);
        masm.Ldp(x29, x30, MemOperand(sp, 16, PostIndex));
        masm.Ret();
    });
    DecodeAndPrint(memory, kGuestBase, "prologue");

    // Block 4: loads/stores with various widths and sign extension.
    BuildGuestCode(memory, [](vixl::aarch64::MacroAssembler& masm) {
        using namespace vixl::aarch64;
        masm.Ldr(w0, MemOperand(x1, 8));
        masm.Ldrb(w2, MemOperand(x1, 3));
        masm.Ldrsw(x3, MemOperand(x1, 12));
        masm.Strh(w4, MemOperand(x1, -2, PreIndex));
        masm.Ldrsh(x5, MemOperand(x1, 6, PostIndex));
        masm.Ldr(x6, MemOperand(x1, x2, LSL, 3));
        masm.Ret();
    });
    DecodeAndPrint(memory, kGuestBase, "load_store");

    // Block 5: bitfield / moves / mul / csel.
    BuildGuestCode(memory, [](vixl::aarch64::MacroAssembler& masm) {
        using namespace vixl::aarch64;
        masm.movz(x0, 0x1234, 16);
        masm.movn(w7, 5);
        masm.movk(x0, 0xabcd);
        masm.Lsr(x1, x0, 4);
        masm.Asr(x2, x0, 8);
        masm.Mul(x3, x0, x1);
        masm.Sdiv(x4, x0, x1);
        masm.Csel(x5, x0, x1, ne);
        masm.Tst(x0, 0xF);
        masm.Ret();
    });
    DecodeAndPrint(memory, kGuestBase, "misc");

    return 0;
}
