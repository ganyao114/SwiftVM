#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/uniform_forward.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/common/host_pair_result.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using O = ir::OpCode;
namespace {
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    Storage() { state->guest_addr_mask = UINT64_MAX; state->guest_addr_limit = UINT64_MAX; }
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
HaltReason Run(ir::Block& block, Storage& storage) {
    block.ReIdInstr(); x64::Translator translator{&block}; translator.Translate();
    return translator.getCode<HaltReason (*)(backend::State*)>()(storage.state);
}
u32 Stores(const ir::Block& block) {
    u32 count{}; for (const auto& inst : block.GetInstList()) count += inst.GetOp() == O::StoreUniform; return count;
}
u64 Mask(u32 size) { return size == 8 ? UINT64_MAX : (u64{1} << (size * 8)) - 1; }
class CodeMemory final : public MemoryInterface {
public:
    explicit CodeMemory(std::initializer_list<u32> words) : words(words) { this->words.push_back(0xd4200000); }
    bool Read(void* dest, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 > words.size() * 4 || size > words.size() * 4 - (address - 0x1000)) return false;
        std::memcpy(dest, reinterpret_cast<u8*>(words.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto offset = reinterpret_cast<size_t>(address) - 0x1000;
        return offset < words.size() * 4 ? reinterpret_cast<u8*>(words.data()) + offset : nullptr;
    }
    std::vector<u32> words;
};
void Decode(ir::Block& block, CodeMemory& memory) {
    ir::Assembler assembler{&block}; swift::arm64::A64Decoder decoder{0x1000, &memory, &assembler}; decoder.Decode();
}
} // namespace

TEST_CASE("x64 local uniform forwarding canonicalizes contained scalar views", "[x64][uniform-forward]") {
    for (auto store_type : {T::U8, T::U16, T::U32, T::U64})
    for (auto load_type : {T::U8, T::U16, T::U32, T::U64, T::S8, T::S16, T::S32, T::S64}) {
        const auto extent = ir::GetValueSizeByte(store_type), size = ir::GetValueSizeByte(load_type);
        if (size > extent) continue;
        for (u32 offset = 0; offset + size <= extent; ++offset) {
            ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
            auto original = as.LoadImm(ir::Imm{u64{0xfedcba9876543210}}).SetType(T::U64);
            // Store descriptor deliberately wider: actual write extent is the input view.
            as.StoreUniform(ir::Uniform{16, T::U64}, original.SetCastType(store_type));
            auto loaded = as.LoadUniform(ir::Uniform{16 + offset, load_type}).SetType(load_type);
            auto full = as.BitCast(loaded).SetType(T::V128);
            as.StoreUniform(ir::Uniform{64, T::V128}, full);
            block.SetTerminal(ir::terminal::ReturnToHost{});
            const auto before = Stores(block);
            const auto stats = x64::ForwardUniformLoads(&block, 4096);
            REQUIRE(stats.loads_seen == 1); REQUIRE(stats.loads_forwarded == 1);
            REQUIRE(Stores(block) == before); REQUIRE(loaded.Def()->GetOp() == O::BitExtract);
            Storage storage; std::memset(storage.state->uniform_buffer_begin + 16, 0xa5, 16);
            REQUIRE(Run(block, storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == ((u64{0xfedcba9876543210} >> (offset * 8)) & Mask(size)));
            REQUIRE(storage.Get(72) == 0);
            for (u32 byte = extent; byte < 16; ++byte) REQUIRE(storage.state->uniform_buffer_begin[16 + byte] == 0xa5);
        }
    }
}

TEST_CASE("x64 local uniform forwarding retains partial overlap loads", "[x64][uniform-forward]") {
    ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
    auto wide = as.LoadImm(ir::Imm{u64{0x8877665544332211}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, wide);
    auto replacement = as.LoadImm(ir::Imm{u16{0xbbaa}}).SetType(T::U16);
    as.StoreUniform(ir::Uniform{18, T::U16}, replacement);
    auto whole = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto low = as.LoadUniform(ir::Uniform{16, T::U16}).SetType(T::U16);
    auto inserted = as.LoadUniform(ir::Uniform{18, T::U16}).SetType(T::U16);
    as.StoreUniform(ir::Uniform{64, T::U64}, whole); as.StoreUniform(ir::Uniform{80, T::U16}, low);
    as.StoreUniform(ir::Uniform{96, T::U16}, inserted); block.SetTerminal(ir::terminal::ReturnToHost{});
    const auto stats = x64::ForwardUniformLoads(&block, 4096);
    REQUIRE(whole.Def()->GetOp() == O::LoadUniform); REQUIRE(stats.loads_forwarded == 2);
    Storage storage; REQUIRE(Run(block, storage) == HaltReason::CallHost);
    REQUIRE(storage.Get(64) == 0x88776655bbaa2211ULL); REQUIRE(storage.Get(80) == 0x2211); REQUIRE(storage.Get(96) == 0xbbaa);
}

TEST_CASE("x64 local uniform forwarding preserves vector load zero initialization", "[x64][uniform-forward]") {
    for (auto type : {T::V8, T::V16, T::V32, T::V64, T::V128}) {
        ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
        auto full = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{32, T::V128}, full);
        auto loaded = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        auto rewidened = as.BitCast(loaded).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, rewidened); block.SetTerminal(ir::terminal::ReturnToHost{});
        const auto stats = x64::ForwardUniformLoads(&block, 4096);
        REQUIRE(stats.loads_forwarded == (type == T::V128));
        REQUIRE(loaded.Def()->GetOp() == (type == T::V128 ? O::BitCast : O::LoadUniform));
        Storage storage;
        for (u32 byte = 0; byte < 16; ++byte) storage.state->uniform_buffer_begin[16 + byte] = u8(0x91 + byte * 7);
        REQUIRE(Run(block, storage) == HaltReason::CallHost);
        const auto size = ir::GetValueSizeByte(type);
        for (u32 byte = 0; byte < 16; ++byte)
            REQUIRE(storage.state->uniform_buffer_begin[64 + byte] == (byte < size ? u8(0x91 + byte * 7) : 0));
    }
}

TEST_CASE("x64 local uniform forwarding invalidates callback and escaped address facts", "[x64][uniform-forward]") {
    ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
    auto initial = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64); as.StoreUniform(ir::Uniform{16, T::U64}, initial);
    auto address = as.GetUniformAddress(ir::Imm{u32{16}}).SetType(T::U64);
    auto before = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto mutate = +[](u64 pointer, u64, u64) -> u64 {
        u64 previous; std::memcpy(&previous, reinterpret_cast<void*>(pointer), 8);
        const u64 next = previous + 35; std::memcpy(reinterpret_cast<void*>(pointer), &next, 8); return previous;
    };
    auto result = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(mutate)}}, address, initial, initial).SetType(T::U64);
    auto after = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, before); as.StoreUniform(ir::Uniform{80, T::U64}, after);
    as.StoreUniform(ir::Uniform{96, T::U64}, result); block.SetTerminal(ir::terminal::ReturnToHost{});
    const auto stats = x64::ForwardUniformLoads(&block, 4096);
    REQUIRE(stats.loads_forwarded == 0); REQUIRE(stats.barriers == 2);
    Storage storage; REQUIRE(Run(block, storage) == HaltReason::CallHost);
    REQUIRE(storage.Get(16) == 42); REQUIRE(storage.Get(64) == 7); REQUIRE(storage.Get(80) == 42); REQUIRE(storage.Get(96) == 7);
}

TEST_CASE("x64 local uniform forwarding does not cross conditional paths", "[x64][uniform-forward]") {
    ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
    auto initial = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64); as.StoreUniform(ir::Uniform{16, T::U64}, initial);
    auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    auto branch = as.Goto(ir::BOOL{condition});
    auto replacement = as.LoadImm(ir::Imm{u64{42}}).SetType(T::U64); as.StoreUniform(ir::Uniform{16, T::U64}, replacement);
    as.BindLabel(branch); auto loaded = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, loaded); block.SetTerminal(ir::terminal::ReturnToHost{});
    const auto stats = x64::ForwardUniformLoads(&block, 4096);
    REQUIRE(stats.loads_forwarded == 0); REQUIRE(loaded.Def()->GetOp() == O::LoadUniform);
    for (u64 value : {u64{0}, u64{1}}) { Storage storage; storage.Put(0, value);
        REQUIRE(Run(block, storage) == HaltReason::CallHost); REQUIRE(storage.Get(64) == (value ? 7 : 42)); }
}

TEST_CASE("x64 local uniform forwarding treats arbitrary Cpuid helpers as opaque", "[x64][uniform-forward]") {
    Storage storage; ir::Block block{ir::Location{0x1000}}; ir::Assembler as{&block};
    auto initial = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64); as.StoreUniform(ir::Uniform{16, T::U64}, initial);
    auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state->uniform_buffer_begin + 16)}).SetType(T::U64);
    auto helper = +[](u64 address, u64, u64) -> HostPairResult {
        const u64 next = 42; std::memcpy(reinterpret_cast<void*>(address), &next, 8); return {13, 17};
    };
    auto result = as.Cpuid(pointer, initial, ir::Imm{u64{0}}, ir::Imm{reinterpret_cast<u64>(helper)}).SetType(T::U64);
    auto after = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, after); as.StoreUniform(ir::Uniform{80, T::U64}, result);
    block.SetTerminal(ir::terminal::ReturnToHost{});
    const auto stats = x64::ForwardUniformLoads(&block, 4096);
    REQUIRE(stats.loads_forwarded == 0); REQUIRE(stats.barriers == 1);
    REQUIRE(Run(block, storage) == HaltReason::CallHost); REQUIRE(storage.Get(64) == 42); REQUIRE(storage.Get(80) == 13);
}

TEST_CASE("x64 local uniform forwarding executes real A64 W X dataflow", "[x64][uniform-forward][arm64]") {
    CodeMemory memory{0x52800540, 0x11000401, 0x91001022}; // mov w0,#42; add w1,w0,#1; add x2,x1,#4
    ir::Block baseline{ir::Location{0x1000}}, optimized{ir::Location{0x1000}};
    Decode(baseline, memory); Decode(optimized, memory);
    Storage expected, actual; std::memset(expected.state->uniform_buffer_begin, 0xa5, sizeof(swift::arm64::ThreadContext64));
    std::memcpy(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, sizeof(swift::arm64::ThreadContext64));
    backend::interp::Interpreter interpreter{*expected.state, &baseline}; interpreter.Run();
    const auto count = Stores(optimized);
    const auto stats = x64::ForwardUniformLoads(&optimized, sizeof(swift::arm64::ThreadContext64));
    REQUIRE(stats.loads_forwarded >= 2); REQUIRE(Stores(optimized) == count);
    REQUIRE(Run(optimized, actual) == HaltReason::CallHost);
    REQUIRE(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, sizeof(swift::arm64::ThreadContext64)) == 0);
    const auto* context = reinterpret_cast<swift::arm64::ThreadContext64*>(actual.state->uniform_buffer_begin);
    REQUIRE(context->r[0] == 42); REQUIRE(context->r[1] == 43); REQUIRE(context->r[2] == 47);
}

TEST_CASE("x64 local uniform forwarding keeps A64 state committed before a memory fault", "[x64][uniform-forward][arm64]") {
    CodeMemory memory{0xd2800020, 0x91000401, 0xf9400062, 0xd2800040}; // mov x0,#1; add x1,x0,#1; ldr x2,[x3]; mov x0,#2
    ir::Block block{ir::Location{0x1000}}; Decode(block, memory);
    const auto count = Stores(block);
    const auto stats = x64::ForwardUniformLoads(&block, sizeof(swift::arm64::ThreadContext64));
    REQUIRE(stats.loads_forwarded >= 1); REQUIRE(Stores(block) == count);
    Storage storage; auto* context = reinterpret_cast<swift::arm64::ThreadContext64*>(storage.state->uniform_buffer_begin);
    context->r[2] = 0xdeadbeef; context->r[3] = 0xabcdef00;
    storage.state->interp_range_check = +[](void*, u64, u64) { return false; };
    REQUIRE(Run(block, storage) == HaltReason::PageFatal);
    REQUIRE(context->r[0] == 1); REQUIRE(context->r[1] == 2); REQUIRE(context->r[2] == 0xdeadbeef);
    REQUIRE(storage.state->current_loc == 0x1008);
}

TEST_CASE("x64 production CompileBlock respects uniform forwarding optimization bit", "[x64][uniform-forward]") {
    for (bool enabled : {false, true}) {
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
                      .backend_isa = kX86_64, .uniform_buffer_size = 4096,
                      .static_program = true,
                      .global_opts = enabled ? Optimizations::UniformElimination : Optimizations::None,
                      .stack_alignment = 16};
        backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
        IntrusivePtr<ir::Block> block = new ir::Block(ir::Location{0x1000}); ir::Assembler as{block.get()};
        auto initial = as.LoadImm(ir::Imm{u64{0xfedcba9876543210}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, initial);
        auto loaded = as.LoadUniform(ir::Uniform{16, T::U32}).SetType(T::U32);
        as.StoreUniform(ir::Uniform{64, T::U32}, loaded); block->SetTerminal(ir::terminal::ReturnToHost{});
        auto* entry = x64::CompileBlock(module, block.get()); REQUIRE(entry != nullptr);
        REQUIRE(loaded.Def()->GetOp() == (enabled ? O::BitExtract : O::LoadUniform));
        REQUIRE(Stores(*block) == 2);
        Storage storage;
        REQUIRE(reinterpret_cast<HaltReason (*)(backend::State*)>(entry)(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(16) == 0xfedcba9876543210ULL); REQUIRE(storage.Get(64) == 0x76543210);
    }
}
