#include "test_support.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/module.h"
#include "runtime/backend/guarded_return_stack.h"
#include "runtime/backend/riscv64/link.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/hir_builder.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

namespace swift::tests::riscv {
using T = ir::ValueType;
namespace {
u64 Helper(u64 value, u64, u64) { return value * 3 + 7; }
struct Guest {
    static constexpr u32 size = 65536;
    u8* bytes = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
    Guest() { Check(bytes != MAP_FAILED, "allocate persistence guest bytes"); }
    ~Guest() { munmap(bytes, size); }
    Config ConfigFor(Optimizations opts) {
        auto cfg = TestConfig(); cfg.memory_base = bytes; cfg.guest_addr_mask = size - 1;
        cfg.loc_end = size; cfg.static_program = false; cfg.global_opts = opts; return cfg;
    }
};
IntrusivePtr<ir::Block> Target(u64 guest, u64 value) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{guest})};
    ir::Assembler as{block.get()};
    auto result = as.LoadImm(ir::Imm{value}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}
IntrusivePtr<ir::Block> Source(u64 guest, u64 target) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{guest})};
    ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto result = as.CallHost(&Helper, input, input, input).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, result);
    block->SetTerminal(ir::terminal::LinkBlock{ir::Location{target}}); return block;
}
void Put(backend::State* state, u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
u64 Get(backend::State* state, u32 offset) { u64 result; std::memcpy(&result, state->uniform_buffer_begin + offset, 8); return result; }
struct Site { u8* rx; backend::LinkSiteKey key; backend::LinkSiteRecord record; };
std::vector<Site> Sites(backend::AddressSpace& space, const std::shared_ptr<backend::Module>& module, const u8* entry) {
    backend::FaultEntry owner;
    Check(module->LookupCodeAllocation(entry, owner), "find full persisted allocation");
    const auto region = module->GetCodeRegion(entry);
    std::vector<Site> result;
    for (auto* pc = owner.host_start; pc < owner.host_end; pc += 4) {
        const backend::LinkSiteKey key{region->id, u32(pc - region->rx_base)};
        if (auto record = space.GetLinkManager().QuerySite(key)) result.push_back({pc, key, *record});
    }
    return result;
}
u32 Word(void* pc) { return std::atomic_ref<u32>(*static_cast<u32*>(pc)).load(); }
struct Env {
    std::string key; std::optional<std::string> previous;
    Env(const char* name, const char* value) : key(name) {
        if (auto* old = GetRawSvmConfigEnvForTest(name)) previous = old;
        SetSvmConfigEnvForTest(name, value, 1);
    }
    ~Env() {
        if (previous) SetSvmConfigEnvForTest(key.c_str(), previous->c_str(), 1);
        else UnsetSvmConfigEnvForTest(key.c_str());
    }
};
void ExitPublication() {
    const std::array<UniformMapDesc, 2> bindings{{{0, 8, 3, false}, {16, 16, 19, true}}};
    for (u32 terminal = 0; terminal < 4; ++terminal) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf000 + terminal})}; ir::Assembler as{block.get()};
        auto scalar = as.LoadImm(ir::Imm{u64{73}}).SetType(T::U64);
        as.SetHostGPR(scalar, ir::Imm{u32{3}}, ir::Imm{u32{0}});
        auto vector = as.LoadUniform(ir::Uniform{64, T::V128}).SetType(T::V128);
        as.SetHostFPR(vector, ir::Imm{u32{19}}, ir::Imm{u32{0}});
        auto lane = as.VecExtract64(vector, ir::Imm{u32{0}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{96, T::U64}, lane);
        auto cond = as.LoadUniform(ir::Uniform{80, T::U8}).SetType(T::U8);
        const auto returned = ir::terminal::ReturnToHost{};
        if (!terminal) block->SetTerminal(returned);
        else if (terminal == 1) block->SetTerminal(ir::terminal::If{ir::BOOL{cond}, returned, returned});
        else if (terminal == 2) block->SetTerminal(ir::terminal::CheckHalt{returned});
        else {
            block->SetTerminal(ir::terminal::Switch{cond, {{ir::Imm{u64{1}}, returned}}});
        }
        Compiled compiled{block.get(), true, rv::HostFeatures::Detect(), bindings};
        u32 publications{};
        const auto* binding = compiled.context.FindUniformBinding(false, 3);
        const auto reg = rv::scalar_registers[binding->gpr_indices[0]].Index();
        const auto& code = compiled.context.GetMasm().GetCodeBuffer();
        for (u32 at = 0; at < compiled.context.CurrentBufferSize(); at += 4) {
            u32 word; std::memcpy(&word, code.GetOffsetPointer(at), 4);
            publications += (word & 0x7f) == 0x23 && ((word >> 20) & 31) == reg && ((word >> 15) & 31) != 2;
        }
        Check(publications == (terminal ? 3u : 2u), "each exit emits one resident GPR publication: terminal=" +
              std::to_string(terminal) + " count=" + std::to_string(publications));
        for (u64 select : {u64{0}, u64{1}}) {
            StateStorage state; state.Put(64, 0x123456789abcdef0ULL); state.Put(72, 0xfedcba9876543210ULL); state.Put(80, select);
            const auto expected = terminal == 2 && select ? HaltReason::CodeMiss
                : terminal == 3 && !select ? HaltReason::None : HaltReason::CallHost;
            if (terminal == 2 && select) state.state->halt_reason = expected;
            Check(compiled.fn(state.state) == expected && state.Get(0) == 73 &&
                  state.Get(16) == state.Get(64) && state.Get(24) == state.Get(72),
                  "all terminal alternatives publish complete resident GPR/RVV values");
        }
    }
}
void LiteralRelocations() {
    auto block = Source(0x100, 0x200);
    Compiled code{block.get()};
    const auto& image = backend::GetHostImage();
    const auto bytes = std::span{code.context.GetMasm().GetCodeBuffer().GetOffsetPointer(0), size_t(code.context.CurrentBufferSize())};
    const auto scan = backend::ScanCodeUnit(bytes, image, 65536, {}, kRiscv64);
    Check(scan.ok && scan.relocs.size() >= 2, "RV64 serializer identifies all ABI host literals");
    std::vector<u8> copy(bytes.begin(), bytes.end());
    std::string error;
    const backend::HostImageInfo shifted{image.base + 0x100000000ULL, image.size};
    Check(backend::ApplyRelocations(copy.data(), copy.size(), scan.relocs, shifted, &error, kRiscv64),
          "RV64 relocation survives a different full-width ASLR slide");
    for (const auto& rel : scan.relocs) {
        u64 value; std::memcpy(&value, copy.data() + rel.code_offset, 8);
        Check(value == shifted.base + rel.addend, "every literal rebases to the new host image");
    }
    copy.assign(bytes.begin(), bytes.end());
    Check(!backend::ApplyRelocations(copy.data(), copy.size(), std::span{scan.relocs}.subspan(1), image, &error, kRiscv64),
          "omitted RV64 pointer relocation is rejected before execution");
    auto bad = scan.relocs; bad[0].code_offset += 8;
    Check(!backend::ApplyRelocations(copy.data(), copy.size(), bad, image, &error, kRiscv64),
          "a relocation cannot invent another literal slot");
    IntrusivePtr<ir::Block> raw{new ir::Block(ir::Location{0x300})}; ir::Assembler as{raw.get()};
    auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&Helper)}).SetType(T::U64);
    (void)as.CallLambda(ir::Lambda{pointer}, pointer, pointer, pointer);
    raw->SetTerminal(ir::terminal::ReturnToHost{}); Compiled unsupported{raw.get()};
    Check(!backend::ScanCodeUnit({unsupported.context.GetMasm().GetCodeBuffer().GetOffsetPointer(0),
            unsupported.context.CurrentBufferSize()}, image, 65536, {}, kRiscv64).ok,
          "untracked absolute host pointer materialization cannot enter disk/AOT code");
    biscuit::Assembler numeric;
    numeric.LI(biscuit::t0, image.base); numeric.SLLI(biscuit::t0, biscuit::t0, 4); numeric.RET();
    Check(backend::ScanCodeUnit({numeric.GetCodeBuffer().GetOffsetPointer(0),
            size_t(numeric.GetCodeBuffer().GetCursorOffset())}, image, 65536, {}, kRiscv64).ok,
          "a numeric immediate prefix inside the host image is not a pointer relocation");
    for (bool arithmetic : {false, true}) {
        biscuit::Assembler shifted;
        shifted.LI(biscuit::t0, image.base);
        if (arithmetic) shifted.SRAI(biscuit::t0, biscuit::t0, 32);
        else shifted.SRLI(biscuit::t0, biscuit::t0, 32);
        shifted.RET();
        Check(backend::ScanCodeUnit({shifted.GetCodeBuffer().GetOffsetPointer(0),
                size_t(shifted.GetCodeBuffer().GetCursorOffset())}, image, 65536, {}, kRiscv64).ok,
              "right-shift integer chains do not retain an image-looking prefix");
    }
    biscuit::Assembler consumed;
    consumed.LI(biscuit::t0, image.base); consumed.MV(biscuit::t1, biscuit::t0);
    consumed.SLLI(biscuit::t0, biscuit::t0, 4); consumed.RET();
    Check(!backend::ScanCodeUnit({consumed.GetCodeBuffer().GetOffsetPointer(0),
            size_t(consumed.GetCodeBuffer().GetCursorOffset())}, image, 65536, {}, kRiscv64).ok,
          "an image pointer consumed before a numeric shift is still rejected");
}
void SmcInstructionBoundary() {
    for (u8 condition : {u8{0}, u8{1}}) {
        Guest guest; auto cfg = guest.ConfigFor(Optimizations::None);
        backend::AddressSpace space{cfg}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        IntrusivePtr<ir::Block> source{new ir::Block(ir::Location{0x1000})}; ir::Assembler as{source.get()};
        auto address = as.LoadImm(ir::Imm{u64{0x1800}}).SetType(T::U64);
        auto byte = as.LoadImm(ir::Imm{u8{91}}).SetType(T::U8);
        as.StoreMemory(ir::Operand{address}, byte);
        auto cond = as.LoadImm(ir::Imm{condition}).SetType(T::U8);
        auto skip = as.NotGoto(ir::BOOL{cond}); as.Nop(); as.BindLabel(skip);
        auto previous = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto next = as.Add(previous, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, next); as.AdvancePC(ir::Imm{u32{1}});
        source->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x1001}});
        Check(module->Push(source.get()) && backend::TranslateIR(module, source), "compile intra-instruction SMC fixture");
        Put(runtime.GetState(), 0, 0); runtime.SetLocation(0x1000);
        Check(runtime.Run() == HaltReason::CodeMiss && runtime.GetLocation() == 0x1001 &&
              guest.bytes[0x1800] == 91 && Get(runtime.GetState(), 0) == 1,
              "SMC completes the guest instruction before exiting across either local branch");
        Check(!space.GetCodeCache(ir::Location{0x1000}), "SMC still retires the modified page's source allocation");
    }
}
void DirectLinks() {
    Guest guest; auto cfg = guest.ConfigFor(Optimizations::BlockLink);
    backend::AddressSpace space{cfg}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    auto source = Source(0x1000, 0x2000);
    Check(module->Push(source.get()), "publish direct-link source");
    auto* entry = static_cast<u8*>(backend::TranslateIR(module, source));
    const auto sites = Sites(space, module, entry);
    Check(sites.size() == 1 && Word(sites[0].rx) == sites[0].record.unlinked_instruction,
          "RV64 link starts as an allocation-local cold JAL");
    Put(runtime.GetState(), 0, 19); runtime.SetLocation(0x1000);
    Check(runtime.Run() == HaltReason::CodeMiss && runtime.GetLocation() == 0x2000,
          "uncompiled direct target falls through to the normal code miss");
    auto target = Target(0x2000, 73);
    Check(module->Push(target.get()) && backend::TranslateIR(module, target), "publish direct-link target");
    runtime.SetLocation(0x1000);
    Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 16) == Helper(19,0,0) &&
          Get(runtime.GetState(), 32) == 73, "cold direct link publishes source effects and reaches target");
    Check(space.GetLinkManager().QuerySite(sites[0].key)->state == backend::LinkSiteState::Linked &&
          (Word(sites[0].rx) & 0xfff) == 0x6f, "near direct edge becomes a single JAL x0");
    const auto calls = space.GetLinkManager().GetStats().linker_calls;
    for (u32 i = 0; i < 16; ++i) {
        runtime.SetLocation(0x1000); Check(runtime.Run() == HaltReason::CallHost, "warm native direct edge executes");
    }
    Check(space.GetLinkManager().GetStats().linker_calls == calls, "warm near links never reenter C++ linking");
#if defined(__riscv) && __riscv_xlen == 64
    runtime.GetState()->halt_reason = HaltReason::None;
    Check(SwiftRiscvCheckABI(reinterpret_cast<BlockFn>(entry), runtime.GetState()) == 1,
          "tail-linked blocks preserve complete LP64D state and balance the stack");
#endif
    runtime.GetState()->halt_reason = HaltReason::None;
    space.GetSmcTracker().InvalidateRange(space, nullptr, 0x2000, 0x2001);
    Check(Word(sites[0].rx) == sites[0].record.unlinked_instruction &&
          space.GetCodeCache(ir::Location{0x2000}) == nullptr, "SMC restores RV64 incoming links before retirement");
    auto replacement = Target(0x2000, 91);
    Check(module->Push(replacement.get()) && backend::TranslateIR(module, replacement), "recompile invalidated link target");
    runtime.SetLocation(0x1000);
    Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 32) == 91,
          "source relinks to the replacement generation");
    Check(space.GetLinkManager().SignalInvalidateTarget(0x2000).found &&
          Word(sites[0].rx) == sites[0].record.unlinked_instruction,
          "signal invalidation restores the RV64 instruction atomically");

}
void FarLinks() {
    Guest guest; auto cfg = guest.ConfigFor(Optimizations::BlockLink);
    backend::AddressSpace space{cfg}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    auto far_source = Source(0x3000, 0x4000);
    Check(module->Push(far_source.get()), "publish far-link source");
    auto* far_entry = static_cast<u8*>(backend::TranslateIR(module, far_source));
    const auto filler = module->AllocCodeCache(2u << 20);
    Check(filler.first != backend::INVALID_CACHE_ID, "separate far target beyond JAL reach");
    auto far_target = Target(0x4000, 123);
    Check(module->Push(far_target.get()) && backend::TranslateIR(module, far_target), "publish far-link target");
    const auto far_sites = Sites(space, module, far_entry);
    runtime.SetLocation(0x3000); Check(runtime.Run() == HaltReason::CallHost, "far link resolves its target");
    Check(space.GetLinkManager().QuerySite(far_sites[0].key)->state == backend::LinkSiteState::Far,
          "far edge selects its allocation-local L2 leaf");
    const auto far_calls = space.GetLinkManager().GetStats().linker_calls;
    runtime.SetLocation(0x3000);
    Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 32) == 123 &&
          space.GetLinkManager().GetStats().linker_calls == far_calls, "warm far link avoids C++ and table hashing");
}
void ReturnStack() {
    Guest guest; auto cfg = guest.ConfigFor(Optimizations::ReturnStackBuffer);
    backend::AddressSpace space{cfg}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    IntrusivePtr<ir::Block> caller{new ir::Block(ir::Location{0x500})}; ir::Assembler push{caller.get()};
    push.PushRSB(ir::Lambda{ir::Imm{u64{0x600}}}); caller->SetTerminal(ir::terminal::ReturnToHost{});
    IntrusivePtr<ir::Block> callee{new ir::Block(ir::Location{0x700})}; ir::Assembler pop{callee.get()};
    auto address = pop.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    pop.SetLocation(ir::Lambda{address}); pop.PopRSB(ir::Lambda{address}); callee->SetTerminal(ir::terminal::PopRSBHint{});
    auto target = Target(0x600, 0x123456);
    Check(module->Push(caller.get()) && module->Push(callee.get()) && module->Push(target.get()), "publish RSB fixtures");
    auto call = reinterpret_cast<BlockFn>(backend::TranslateIR(module, caller));
    auto ret = reinterpret_cast<BlockFn>(backend::TranslateIR(module, callee));
    Check(call && ret && backend::TranslateIR(module, target), "compile native RSB push and return terminal");
    auto* state = runtime.GetState(); Put(state, 0, 0x600);
    const auto reset = [&] { state->halt_reason = HaltReason::None; };
    Check(call(state) == HaltReason::CallHost && state->rsb_pointer + 1 == state->rsb_empty,
          "RSB push stores the return guest PC and stable dispatch index");
    reset(); Check(ret(state) == HaltReason::CallHost && Get(state, 32) == 0x123456 && state->rsb_pointer == state->rsb_empty,
          "RSB hit tail-calls a compiled return entry without the dispatcher");
    reset(); Check(ret(state) == HaltReason::None && state->rsb_pointer == state->rsb_empty,
          "empty RSB return takes an ordinary dispatch fallback");
    reset(); (void)call(state); reset(); Put(state, 0, 0x601);
    Check(ret(state) == HaltReason::None && state->rsb_pointer == state->rsb_empty,
          "mismatching architectural return address discards predictions");
    Put(state, 0, 0x600); reset(); (void)call(state); reset();
    state->rsb_pointer->dispatch_index = UINT64_MAX;
    Check(ret(state) == HaltReason::None && state->rsb_pointer == state->rsb_empty,
          "invalid predicted slot cannot escape the L2 table");
    state->rsb_pointer = reinterpret_cast<backend::RSBFrame*>(reinterpret_cast<u8*>(state->rsb_empty) - backend::GuardedReturnStack::kUsableSize / 2);
    reset(); Check(call(state) == HaltReason::CallHost && state->rsb_pointer + 1 == state->rsb_empty,
          "RSB overflow resets before touching its guard page");
    reset(); Check(ret(state) == HaltReason::CallHost, "RSB remains usable after overflow");
    state->rsb_pointer = state->rsb_empty + 2;
    reset(); Check(call(state) == HaltReason::CallHost && state->rsb_pointer + 1 == state->rsb_empty,
          "RSB push repairs an out-of-range upper pointer before writing");
    reset(); Check(ret(state) == HaltReason::CallHost, "RSB remains usable after upper-pointer repair");
#if defined(__riscv) && __riscv_xlen == 64
    reset(); (void)call(state); reset();
    Check(SwiftRiscvCheckABI(ret, state) == 1, "RSB tail return preserves LP64D");
#endif
}
void DiskCache() {
    char name[] = "/tmp/swiftvm-rv64-disk-XXXXXX";
    Check(mkdtemp(name) != nullptr, "create isolated disk cache");
    Env env{"SVM_JIT_CACHE", name};
    {
        Guest guest; auto cfg = guest.ConfigFor(Optimizations::BlockLink | Optimizations::ReturnStackBuffer);
        backend::AddressSpace space{cfg}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        Check(space.GetJitDiskCache() && space.GetJitDiskCache()->Enabled(), "RV64 disk cache activates for a biased guest");
        auto source = Source(0x100, 0x200), target = Target(0x200, 77);
        source->AddGuestCodeDependency(ir::Location{0x8000}, ir::Location{0x8001});
        ir::Assembler push{source.get()}; push.PushRSB(ir::Lambda{ir::Imm{u64{0x200}}});
        IntrusivePtr<ir::Block> returned{new ir::Block(ir::Location{0x700})}; ir::Assembler pop{returned.get()};
        auto actual = pop.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        pop.SetLocation(ir::Lambda{actual}); pop.PopRSB(ir::Lambda{actual});
        returned->SetTerminal(ir::terminal::PopRSBHint{});
        Check(module->Push(returned.get()) && backend::TranslateIR(module, returned), "cache native RSB return code");
        IntrusivePtr<ir::Block> memory{new ir::Block(ir::Location{0x300})}; ir::Assembler load{memory.get()};
        auto address = load.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto value = load.LoadMemory(ir::Operand{address}).SetType(T::U64);
        load.StoreUniform(ir::Uniform{64, T::U64}, value);
        memory->SetTerminal(ir::terminal::ReturnToHost{});
        const u64 data = 0x1357acdf2468bdefULL;
        std::memcpy(guest.bytes + 0x9000, &data, 8);
        Check(module->Push(memory.get()) && backend::TranslateIR(module, memory), "cache ordinary memory code and its lease/recovery metadata");
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x4000});
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        using Wide = ir::TypedValue<T::V256>;
        auto wide = function->LoadUniform<Wide>(ir::Uniform{96, T::V256});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x5000}});
        builder.SetCurBlock(ir::Location{0x5000});
        auto sum = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{17}}});
        auto doubled = function->VecAdd<Wide>(wide, wide, ir::Imm{u8{64}});
        function->StoreUniform(ir::Uniform{72, T::U64}, sum);
        function->StoreUniform(ir::Uniform{128, T::V256}, doubled);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        Check(backend::TranslateIR(module, function), "cache shared SSA frame and guest-only interior metadata");
        Check(module->Push(source.get()) && module->Push(target.get()), "publish cold-cache fixtures");
        Check(backend::TranslateIR(module, source) && backend::TranslateIR(module, target), "compile relocatable disk-cache units");
        runtime.SetLocation(0x100); Put(runtime.GetState(), 0, 9);
        Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 32) == 77, "cold cached source actually links");
        space.GetJitDiskCache()->Save();
        Check(space.GetJitDiskCache()->Stats().units_stored == 5 && space.GetJitDiskCache()->Stats().reject_scan == 0,
              "native blocks and shared SSA units, including host literals and linked sites, persist");
    }
    {
        Guest guest; auto cfg = guest.ConfigFor(Optimizations::BlockLink | Optimizations::ReturnStackBuffer);
        const u64 data = 0x1357acdf2468bdefULL; std::memcpy(guest.bytes + 0x9000, &data, 8);
        backend::AddressSpace space{cfg}; Runtime runtime{&space}; space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 5, "fresh address space revives all RV64 units");
        auto module = space.GetDefaultModule(); auto* entry = static_cast<u8*>(space.GetCodeCache(ir::Location{0x100}));
        const auto sites = Sites(space, module, entry);
        Check(sites.size() == 1 && sites[0].record.state == backend::LinkSiteState::Unlinked &&
              Word(sites[0].rx) == sites[0].record.unlinked_instruction, "disk reload rebuilds fresh unlinked metadata");
        runtime.SetLocation(0x100); Put(runtime.GetState(), 0, 11);
        Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 16) == Helper(11,0,0) &&
              Get(runtime.GetState(), 32) == 77, "revived helpers and direct links execute correctly");
        Put(runtime.GetState(), 0, 0x200); runtime.GetState()->halt_reason = HaltReason::None;
        const auto returned = reinterpret_cast<BlockFn>(space.GetCodeCache(ir::Location{0x700}));
        Check(returned(runtime.GetState()) == HaltReason::CallHost && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
              "revived RSB push/pop use the replayed stable dispatch slot");
        runtime.GetState()->halt_reason = HaltReason::None;
        runtime.SetLocation(0x300); Put(runtime.GetState(), 0, 0x9000);
        Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 64) == data,
              "revived memory literals address the current protocol globals");
        runtime.SetLocation(0x300); Put(runtime.GetState(), 0, Guest::size - 4);
        Check(runtime.Run() == HaltReason::PageFatal && runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
              "revived bounds fault releases its memory lease");
        Check(mprotect(guest.bytes + 0x9000, 4096, PROT_NONE) == 0, "protect revived memory fixture");
        runtime.SetLocation(0x300); Put(runtime.GetState(), 0, 0x9000);
        Check(runtime.Run() == HaltReason::PageFatal && runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0,
              "revived native fault recovers through allocation-relative metadata and releases its lease");
        Check(mprotect(guest.bytes + 0x9000, 4096, PROT_READ | PROT_WRITE) == 0, "unprotect revived memory fixture");
        runtime.SetLocation(0x4000); Put(runtime.GetState(), 0, 99);
        for (u32 i = 0; i < 4; ++i) Put(runtime.GetState(), 96 + i * 8, 19 + i);
        Check(runtime.Run() == HaltReason::CallHost && Get(runtime.GetState(), 72) == 116 &&
              !space.GetCodeCache(ir::Location{0x5000}), "shared SSA revival publishes only its root ABI entry");
        for (u32 i = 0; i < 4; ++i)
            Check(Get(runtime.GetState(), 128 + i * 8) == 38 + i * 2, "revived shared V256 SSA doubles all four lanes");
        Check(space.GetJitDiskCache()->Stats().units_compiled == 0, "warm-cache execution does not recompile fixtures");
        space.GetSmcTracker().InvalidateRange(space, nullptr, 0x5000, 0x5001);
        Check(!space.GetCodeCache(ir::Location{0x4000}), "SMC of an unpublished SSA interior retires the revived owner");
    }
    std::vector<std::pair<std::filesystem::path, std::vector<char>>> stored;
    for (const auto& file : std::filesystem::directory_iterator(name)) {
        std::ifstream stream{file.path(), std::ios::binary};
        stored.emplace_back(file.path(), std::vector<char>{std::istreambuf_iterator<char>{stream}, {}});
    }
    {
        Guest guest; guest.bytes[0x100] = 1;
        auto cfg = guest.ConfigFor(Optimizations::BlockLink | Optimizations::ReturnStackBuffer);
        backend::AddressSpace space{cfg}; space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().reject_guest_bytes == 1 &&
              space.GetCodeCache(ir::Location{0x100}) == nullptr, "changed guest bytes reject only their cached unit");
    }
    for (const auto& [path, bytes] : stored) {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc}; stream.write(bytes.data(), bytes.size());
    }
    {
        Guest guest; guest.bytes[0x8000] = 1;
        auto cfg = guest.ConfigFor(Optimizations::BlockLink | Optimizations::ReturnStackBuffer);
        backend::AddressSpace space{cfg}; space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().reject_guest_bytes == 1 &&
              !space.GetCodeCache(ir::Location{0x100}), "changed decode dependency rejects its cached owner");
    }
    std::filesystem::remove_all(name);
}
} // namespace
void RuntimePersistence() {
    ExitPublication(); LiteralRelocations(); SmcInstructionBoundary(); DirectLinks(); FarLinks(); ReturnStack(); DiskCache();
    std::cout << "PASS RV64 relocation, direct linking, RSB and disk revival\n";
}
} // namespace swift::tests::riscv
