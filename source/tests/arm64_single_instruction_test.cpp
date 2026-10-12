#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/backend/address_space.h"
#include "translator/arm64/translator.h"
#if defined(SWIFT_HAS_X64_BACKEND)
#include "runtime/backend/x64/translator.h"
#endif

using namespace swift;
using namespace swift::runtime;

namespace {
using namespace vixl::aarch64;
using Context = swift::arm64::ThreadContext64;
using Reason = swift::arm64::InterruptReason;
using Bytes = std::array<u8, 32>;
constexpr VAddr kPC = 0x1000;
constexpr u64 kNZCV = 0xF0000000;
constexpr u64 kOtherFlags = 0xA5A5000002345678;

// The host buffer is deliberately unaligned, and the following guest word
// mutates x20 if a decoder accidentally crosses the instruction boundary.
class FetchMemory final : public MemoryInterface {
public:
    FetchMemory(u32 word, VAddr base, size_t available = 8)
        : base(base), available(available) {
        const u32 next = 0xD2800020u | 20u; // MOV x20, #1
        std::memcpy(storage.data() + 1, &word, 4);
        std::memcpy(storage.data() + 5, &next, 4);
    }
    bool Read(void* dest, size_t address, size_t size) override {
        ++data_reads;
        return Copy(dest, address, size);
    }
    CodeFetchResult ReadCode(void* dest, size_t address, size_t size) override {
        reads.push_back({address, size});
        if (fetch_result != CodeFetchResult::Success) return fetch_result;
        return Copy(dest, address, size) ? CodeFetchResult::Success : CodeFetchResult::Fault;
    }
    bool MatchesCodeAddressSpace(std::uintptr_t bias, std::uint64_t mask) const override {
        return bias == identity_bias && mask == identity_mask;
    }
    bool Copy(void* dest, size_t address, size_t size) {
        if (address < base || address - base > available ||
            size > available - (address - base)) return false;
        std::memcpy(dest, storage.data() + 1 + address - base, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        ++pointer_reads;
        const auto offset = reinterpret_cast<size_t>(address) - base;
        return offset < available ? storage.data() + 1 + offset : nullptr;
    }
    struct ReadCall { size_t address; size_t size; };
    std::vector<ReadCall> reads;
    size_t pointer_reads{};
    size_t data_reads{};
    CodeFetchResult fetch_result{CodeFetchResult::Success};
    std::uintptr_t identity_bias{};
    std::uint64_t identity_mask{UINT64_MAX};
private:
    std::array<u8, 9> storage{};
    VAddr base;
    size_t available;
};

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context) + 32> storage{};
    backend::State* state = new (storage.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
};

Context Initial() {
    Context result{};
    for (u32 i = 0; i < result.r.size(); ++i)
        result.r[i] = 0x8123456789ABCDEFULL ^ (u64(i) * 0x0101010101010101ULL);
    result.fp = 0xFEDCBA9876543210ULL;
    result.lr = 0x9988776655443322ULL;
    result.sp = 0x1234567887654321ULL;
    result.pc = 0x777788889999AAAAULL;
    result.pstate = 0x13579BDF;
    result.padding = 0x2468ACE0;
    result.fpcr = 0x00800000;
    result.fpsr = 0x08000015;
    result.tpidr = 0x1020304050607080ULL;
    for (u32 i = 0; i < 32; ++i) {
        const std::array<u64, 2> lanes{0x1122334455667788ULL + i,
                                      0x8877665544332211ULL - i};
        std::memcpy(&result.v[i], lanes.data(), sizeof(lanes));
    }
    return result;
}

struct Program {
    FetchMemory memory;
    ir::Block block;
    ir::Assembler assembler{&block};
    swift::arm64::A64Decoder decoder;
    CodeFetchResult fetch_status{};
#if defined(SWIFT_HAS_X64_BACKEND)
    std::array<std::unique_ptr<backend::x64::Translator>, 2> native;
#endif
    Program(u32 word, VAddr start = kPC, size_t available = 8)
        : memory(word, start, available), block(0, start), decoder(start, &memory, &assembler) {
        fetch_status = decoder.DecodeSingle();
#if defined(SWIFT_HAS_X64_BACKEND)
        for (u32 cache = 0; cache < 2; ++cache) {
            native[cache] = std::make_unique<backend::x64::Translator>(
                    &block, backend::x64::HostFeatures::Detect(), nullptr, nullptr, bool(cache));
            native[cache]->Translate();
        }
#endif
    }
    void CheckFetch(bool fetched = true, bool attempted = true) {
        REQUIRE(fetch_status == (fetched ? CodeFetchResult::Success : CodeFetchResult::Fault));
        REQUIRE(memory.pointer_reads == 0);
        REQUIRE(memory.data_reads == 0);
        REQUIRE(memory.reads.size() == size_t(attempted));
        if (attempted) {
            REQUIRE(memory.reads[0].address == block.GetStartLocation().Value());
            REQUIRE(memory.reads[0].size == 4);
        }
        const auto& deps = block.GetGuestCodeDependencies();
        REQUIRE(deps.size() == size_t(fetched));
        if (fetched) {
            REQUIRE(deps[0].start == block.GetStartLocation());
            REQUIRE(deps[0].end.Value() == block.GetStartLocation().Value() + 4);
        }
        for (auto& inst : block.GetInstList()) REQUIRE(inst.GetOp() != ir::OpCode::AdvancePC);
    }
    void Check(const Context& initial, const Context& expected, u64 flags, u64 expected_flags,
               VAddr pc, HaltReason halt = HaltReason::None,
               Bytes input = {}, Bytes wanted = {}) {
        for (u32 route = 0; route <
#if defined(SWIFT_HAS_X64_BACKEND)
             3
#else
             1
#endif
             ; ++route) {
            CAPTURE(route, pc);
            Machine machine;
            machine.Guest() = initial;
            alignas(16) Bytes actual = input;
            machine.state->pt = actual.data();
            machine.state->guest_addr_limit = actual.size();
            machine.state->host_cpu_flags = flags;
            machine.state->current_loc = ir::Location{0xDEAD0000};
            HaltReason result;
            if (route == 0) result = backend::interp::Interpreter{*machine.state, &block}.Run();
#if defined(SWIFT_HAS_X64_BACKEND)
            else result = native[route - 1]->getCode<HaltReason (*)(backend::State*)>()(machine.state);
#endif
            REQUIRE(result == halt);
            REQUIRE(machine.state->halt_reason == halt);
            REQUIRE(std::memcmp(&machine.Guest(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == expected_flags);
            REQUIRE(machine.state->current_loc.Value() == pc);
            REQUIRE(actual == wanted);
        }
    }
};

u64 WidthMask(bool wide) { return wide ? ~u64{0} : 0xFFFFFFFFULL; }
struct Arithmetic { u64 value; u64 flags; };
Arithmetic Calculate(bool wide, bool subtract, u64 lhs, u64 rhs, bool carry) {
    const u32 bits = wide ? 64 : 32;
    const u64 mask = WidthMask(wide);
    lhs &= mask; rhs &= mask;
    const unsigned __int128 a = lhs, b = rhs;
    const unsigned __int128 extra = subtract ? !carry : carry;
    const auto raw = subtract ? a - b - extra : a + b + extra;
    const u64 result = u64(raw) & mask;
    const u64 sign = u64{1} << (bits - 1);
    const auto signed_value = [=](u64 value) -> __int128 {
        return value & sign ? __int128(value) - (__int128{1} << bits) : __int128(value);
    };
    const __int128 mathematical = subtract
            ? signed_value(lhs) - signed_value(rhs) - __int128(extra)
            : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    const bool overflow = mathematical < -(__int128{1} << (bits - 1)) ||
                          mathematical >= (__int128{1} << (bits - 1));
    const bool c = subtract ? a >= b + extra : (raw >> bits) != 0;
    return {result, (u64(bool(result & sign)) << 31) | (u64(result == 0) << 30) |
                    (u64(c) << 29) | (u64(overflow) << 28)};
}

u32 Relative(u32 base, s32 immediate, u32 rd) {
    const u32 field = u32(immediate) & 0x1FFFFF;
    return base | ((field & 3) << 29) | ((field >> 2) << 5) | rd;
}
} // namespace

TEST_CASE("ARM64 single decode fetches one complete word and never the successor", "[arm64][single]") {
    for (size_t available : {size_t{4}, size_t{8}}) {
        Program program{0x91001420, kPC, available}; // ADD x0,x1,#5
        program.CheckFetch();
        auto initial = Initial(), expected = initial;
        expected.r[0] = initial.r[1] + 5;
        program.Check(initial, expected, kOtherFlags | kNZCV, kOtherFlags | kNZCV, kPC + 4);
        const auto count = program.block.GetInstList().size();
        program.decoder.DecodeSingle(); // already-closed assembler remains untouched
        REQUIRE(program.block.GetInstList().size() == count);
        program.CheckFetch();
    }
    Program nop{0xD503201F};
    nop.CheckFetch();
    nop.Check(Initial(), Initial(), kOtherFlags, kOtherFlags, kPC + 4);
}

TEST_CASE("ARM64 single decode rejects incomplete and misaligned fetches", "[arm64][single]") {
    for (size_t available = 0; available < 4; ++available) {
        Program program{0xD503201F, kPC, available};
        program.CheckFetch(false, true);
        auto expected = Initial(); expected.padding = u32(Reason::PAGE_FATAL);
        program.Check(Initial(), expected, kOtherFlags, kOtherFlags, kPC, HaltReason::CallHost);
    }
    for (VAddr offset : {VAddr{1}, VAddr{2}, VAddr{3}}) {
        Program program{0xD503201F, kPC + offset};
        program.CheckFetch(false, false);
        auto expected = Initial(); expected.padding = u32(Reason::ILL_CODE);
        program.Check(Initial(), expected, kOtherFlags, kOtherFlags, kPC + offset, HaltReason::CallHost);
    }
    constexpr VAddr last_word = ~VAddr{0} - 3;
    Program wrapped{0xD503201F, last_word};
    wrapped.CheckFetch(false, false);
    auto expected = Initial(); expected.padding = u32(Reason::PAGE_FATAL);
    wrapped.Check(Initial(), expected, kOtherFlags, kOtherFlags, last_word, HaltReason::CallHost);
}

TEST_CASE("ARM64 single PC relative instructions use the guest instruction address", "[arm64][single]") {
    for (bool page : {false, true}) for (s32 immediate : {-3, 5}) {
        constexpr VAddr start = 0x8FFC;
        Program program{Relative(page ? u32(ADRP) : u32(ADR), immediate, 0), start};
        program.CheckFetch();
        auto expected = Initial();
        expected.r[0] = page ? (start & ~VAddr{0xFFF}) + s64(immediate) * 4096
                            : start + s64(immediate);
        program.Check(Initial(), expected, kOtherFlags, kOtherFlags, start + 4);
    }
    Program literal{0x58000000u | ((u32(-1022) & 0x7FFFF) << 5)}; // LDR x0,[pc-4088]
    literal.CheckFetch();
    Bytes bytes{}; const u64 value = 0xDEADBEEF12345678ULL;
    std::memcpy(bytes.data() + 8, &value, 8);
    auto expected = Initial(); expected.r[0] = value;
    literal.Check(Initial(), expected, kOtherFlags, kOtherFlags, kPC + 4,
                  HaltReason::None, bytes, bytes);
}

TEST_CASE("ARM64 single decode preserves branch call and return terminals", "[arm64][single]") {
    for (bool call : {false, true}) {
        Program program{call ? 0x97FFFFFEu : 0x14000003u};
        program.CheckFetch(); auto expected = Initial();
        if (call) expected.lr = kPC + 4;
        program.Check(Initial(), expected, kOtherFlags, kOtherFlags, call ? kPC - 8 : kPC + 12);
    }
    for (u32 word : {0xD61F0040u, 0xD63F0040u, 0xD65F0040u}) { // BR/BLR/RET x2
        Program program{word}; program.CheckFetch();
        auto initial = Initial(), expected = initial;
        initial.r[2] = expected.r[2] = 0x87654320;
        if (word == 0xD63F0040) expected.lr = kPC + 4;
        program.Check(initial, expected, kOtherFlags, kOtherFlags, initial.r[2]);
    }
    for (u32 word : {0x54000060u, 0xB4000061u, 0x36380061u}) { // B.EQ/CBZ x1/TBZ x1,#7,+12
        Program program{word}; program.CheckFetch();
        for (bool taken : {false, true}) {
            auto initial = Initial(); initial.r[1] = taken ? 0 : 0x80;
            const u64 flags = kOtherFlags | (taken ? u64{1} << 30 : 0);
            program.Check(initial, initial, flags, flags, taken ? kPC + 12 : kPC + 4);
        }
    }
}

TEST_CASE("ARM64 single decode preserves host exits invalid code and synchronization", "[arm64][single]") {
    struct Case { u32 word; Reason reason; bool next; };
    for (const auto item : {Case{0xD4000001, Reason::SVC, true},
                           Case{0xD4200000, Reason::BRK, false},
                           Case{0xD4400000, Reason::HLT, false},
                           Case{0xD4000002, Reason::FALLBACK, false}, // HVC
                           Case{0, Reason::ILL_CODE, false}}) {
        Program program{item.word}; program.CheckFetch();
        auto expected = Initial(); expected.padding = u32(item.reason);
        program.Check(Initial(), expected, kOtherFlags, kOtherFlags,
                      item.next ? kPC + 4 : kPC, HaltReason::CallHost);
    }
    Program isb{0xD5033FDF}; isb.CheckFetch();
    isb.Check(Initial(), Initial(), kOtherFlags, kOtherFlags, kPC + 4);
}

TEST_CASE("ARM64 single carry arithmetic and conditional compare commit all NZCV", "[arm64][single]") {
    for (bool wide : {false, true}) for (bool subtract : {false, true}) {
        const u64 sign = u64{1} << (wide ? 63 : 31);
        const std::array<std::array<u64, 2>, 4> operands{{{sign - 1, 0}, {0, 0},
                                                       {WidthMask(wide), 1}, {sign, 1}}};
        Program carry{u32(wide ? (subtract ? SBCS_x : ADCS_x) : (subtract ? SBCS_w : ADCS_w)) |
                      (2u << 16) | (1u << 5)};
        carry.CheckFetch();
        for (bool incoming : {false, true}) for (const auto pair : operands) {
            auto initial = Initial();
            initial.r[1] = pair[0]; initial.r[2] = pair[1];
            const auto arithmetic = Calculate(wide, subtract, pair[0], pair[1], incoming);
            auto expected = initial; expected.r[0] = arithmetic.value;
            carry.Check(initial, expected, kOtherFlags | (u64(incoming) << 29) | (u64{1} << 30),
                        kOtherFlags | arithmetic.flags, kPC + 4);
        }
        for (bool immediate : {false, true}) {
            const u32 base = immediate
                    ? u32(wide ? (subtract ? CCMP_x_imm : CCMN_x_imm) : (subtract ? CCMP_w_imm : CCMN_w_imm))
                    : u32(wide ? (subtract ? CCMP_x : CCMN_x) : (subtract ? CCMP_w : CCMN_w));
            constexpr u32 fallback = 0xB;
            Program compare{base | ((immediate ? 3u : 2u) << 16) | (1u << 5) | fallback};
            compare.CheckFetch();
            u32 gotos{}, labels{};
            for (auto& inst : compare.block.GetInstList()) {
                gotos += inst.GetOp() == ir::OpCode::Goto;
                labels += inst.GetOp() == ir::OpCode::BindLabel;
            }
            REQUIRE(gotos == 2); REQUIRE(labels == 2);
            for (bool condition : {false, true}) for (const auto pair : operands) {
                auto initial = Initial(); initial.r[1] = pair[0]; initial.r[2] = pair[1];
                const auto result = Calculate(wide, subtract, pair[0], immediate ? 3 : pair[1], subtract);
                compare.Check(initial, initial, kOtherFlags | (u64(condition) << 30),
                              kOtherFlags | (condition ? result.flags : u64(fallback) << 28), kPC + 4);
            }
        }
    }
}

TEST_CASE("ARM64 single conditional select retains W zero extension and both paths", "[arm64][single]") {
    for (bool wide : {false, true}) {
        Program program{u32(wide ? CSEL_x : CSEL_w) | (2u << 16) | (1u << 5)};
        program.CheckFetch();
        for (bool condition : {false, true}) {
            auto initial = Initial(), expected = initial;
            expected.r[0] = initial.r[condition ? 1 : 2] & WidthMask(wide);
            const u64 flags = kOtherFlags | (u64(condition) << 30);
            program.Check(initial, expected, flags, flags, kPC + 4);
        }
    }
}

TEST_CASE("ARM64 single LSE retains alignment transformation atomic and result stores", "[arm64][single]") {
    for (u32 size = 0; size < 4; ++size) for (u32 ordering = 0; ordering < 4; ++ordering) {
        // LDCLRx: complement the source before AtomicRMW, then write old memory
        // to a deliberately aliased source/result register.
        const u32 word = 0x38201000u | (size << 30) | ((ordering & 1) << 23) |
                         ((ordering >> 1) << 22) | (2u << 5);
        Program program{word}; program.CheckFetch();
        bool alignment{}, atomic{}, store{};
        for (auto& inst : program.block.GetInstList()) {
            alignment |= inst.GetOp() == ir::OpCode::CheckMemoryAlignment;
            atomic |= inst.GetOp() == ir::OpCode::AtomicRMW;
            store |= inst.GetOp() == ir::OpCode::StoreUniform;
        }
        REQUIRE(alignment); REQUIRE(atomic); REQUIRE(store);
        Bytes input; input.fill(0xA7);
        const u64 old = 0xFEDCBA9876543210ULL, operand = 0x0F0FF0F05555AAAAULL;
        const size_t bytes = size_t{1} << size;
        std::memcpy(input.data() + 8, &old, bytes);
        Bytes wanted = input;
        const u64 mask = size == 3 ? ~u64{0} : (u64{1} << (8u << size)) - 1;
        const u64 replacement = old & ~operand;
        std::memcpy(wanted.data() + 8, &replacement, bytes);
        auto initial = Initial(), expected = initial;
        initial.r[0] = expected.r[0] = operand;
        initial.r[2] = expected.r[2] = 8;
        expected.r[0] = old & mask;
        program.Check(initial, expected, kOtherFlags | kNZCV, kOtherFlags | kNZCV,
                      kPC + 4, HaltReason::None, input, wanted);
    }
}

TEST_CASE("ARM64 single safe fetch explicitly rejects absent providers without decoding", "[arm64][single][fetch]") {
    class UnsafeDataMemory final : public MemoryInterface {
    public:
        bool Read(void*, size_t, size_t) override { ++unsafe_reads; return true; }
        bool Write(void*, size_t, size_t) override { return false; }
        void* GetPointer(void*) override { ++unsafe_pointers; return reinterpret_cast<void*>(1); }
        u32 unsafe_reads{}, unsafe_pointers{};
    } memory;
    std::array<u8, 4> bytes{1, 2, 3, 4};
    REQUIRE(memory.ReadCode(bytes.data(), kPC, bytes.size()) == CodeFetchResult::Unsupported);
    REQUIRE((bytes == std::array<u8, 4>{1, 2, 3, 4}));
    REQUIRE_FALSE(memory.MatchesCodeAddressSpace(0, UINT64_MAX));
    for (MemoryInterface* provider : {static_cast<MemoryInterface*>(&memory), static_cast<MemoryInterface*>(nullptr)}) {
        ir::Block block{0, kPC}; ir::Assembler assembler{&block};
        swift::arm64::A64Decoder decoder{kPC, provider, &assembler};
        const auto end = block.GetEndLocation();
        const auto terminal = block.GetTerminal().which();
        REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Unsupported);
        REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Unsupported);
        REQUIRE(block.GetInstList().empty());
        REQUIRE(block.GetGuestCodeDependencies().empty());
        REQUIRE(block.GetEndLocation() == end);
        REQUIRE(block.GetTerminal().which() == terminal);
        REQUIRE_FALSE(assembler.EndCommit());
    }
    REQUIRE(memory.unsafe_reads == 0); REQUIRE(memory.unsafe_pointers == 0);
}

TEST_CASE("ARM64 single unsupported fetch can retry at the original PC", "[arm64][single][fetch]") {
    FetchMemory memory{0x91001420, kPC};
    memory.fetch_result = CodeFetchResult::Unsupported;
    ir::Block block{0, kPC}; ir::Assembler assembler{&block};
    swift::arm64::A64Decoder decoder{kPC, &memory, &assembler};
    REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Unsupported);
    REQUIRE(block.GetInstList().empty()); REQUIRE(block.GetGuestCodeDependencies().empty());
    REQUIRE_FALSE(assembler.EndCommit());
    memory.fetch_result = CodeFetchResult::Success;
    REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Success);
    REQUIRE(memory.reads.size() == 2);
    for (const auto& call : memory.reads) { REQUIRE(call.address == kPC); REQUIRE(call.size == 4); }
    REQUIRE(memory.data_reads == 0); REQUIRE(memory.pointer_reads == 0);
    REQUIRE(block.GetGuestCodeDependencies().size() == 1);
    REQUIRE(block.GetGuestCodeDependencies()[0].start.Value() == kPC);
    REQUIRE(block.GetGuestCodeDependencies()[0].end.Value() == kPC + 4);
    Machine machine; machine.Guest() = Initial();
    backend::interp::Interpreter interpreter{*machine.state, &block};
    REQUIRE(interpreter.Run() == HaltReason::None);
    REQUIRE(machine.Guest().r[0] == Initial().r[1] + 5);
    REQUIRE(machine.Guest().r[20] == Initial().r[20]);
    REQUIRE(machine.state->current_loc.Value() == kPC + 4);
}

TEST_CASE("ARM64 instances own distinct immutable code fetch providers", "[arm64][single][fetch][instance]") {
    using Instance = swift::translator::arm64::Arm64Instance;
    FetchMemory first{0x91001420, kPC}, second{0xD503201F, kPC};
    first.identity_bias = 0x20000; first.identity_mask = 0xffff;
    second.identity_bias = 0x40000; second.identity_mask = 0xffffff;
    auto deleter = [](Instance* instance) { Instance::Destroy(instance); };
    std::unique_ptr<Instance, decltype(deleter)> a{
        Instance::Make(reinterpret_cast<void*>(first.identity_bias), first.identity_mask, &first), deleter};
    std::unique_ptr<Instance, decltype(deleter)> b{
        Instance::Make(reinterpret_cast<void*>(second.identity_bias), second.identity_mask, &second), deleter};
    REQUIRE(a); REQUIRE(b);
    u32 word{};
    REQUIRE(a->GetAddressSpace()->GetConfig().memory->ReadCode(&word, kPC, 4) == CodeFetchResult::Success);
    REQUIRE(word == 0x91001420);
    REQUIRE(b->GetAddressSpace()->GetConfig().memory->ReadCode(&word, kPC, 4) == CodeFetchResult::Success);
    REQUIRE(word == 0xD503201F);
    REQUIRE(a->GetAddressSpace()->GetConfig().memory->ReadCode(&word, kPC, 4) == CodeFetchResult::Success);
    REQUIRE(word == 0x91001420);
    REQUIRE(first.reads.size() == 2); REQUIRE(second.reads.size() == 1);
    REQUIRE(first.data_reads == 0); REQUIRE(second.data_reads == 0);
    REQUIRE(first.pointer_reads == 0); REQUIRE(second.pointer_reads == 0);
    REQUIRE_THROWS_AS(Instance::Make(reinterpret_cast<void*>(first.identity_bias + 0x1000),
                                   first.identity_mask, &first), std::invalid_argument);
    REQUIRE_THROWS_AS(Instance::Make(reinterpret_cast<void*>(first.identity_bias),
                                   first.identity_mask >> 1, &first), std::invalid_argument);
    REQUIRE(first.reads.size() == 2);
    std::unique_ptr<Instance, decltype(deleter)> legacy{Instance::Make(), deleter};
    REQUIRE(legacy);
    REQUIRE(legacy->GetAddressSpace()->GetConfig().memory->ReadCode(&word, kPC, 4) == CodeFetchResult::Unsupported);
}

TEST_CASE("ARM64 registered single decode service constructs unpublished instruction and fault units", "[arm64][single][service]") {
    using Instance = swift::translator::arm64::Arm64Instance;
    FetchMemory memory{0x91001420, kPC}; // ADD x0,x1,#5
    auto deleter = [](Instance* instance) { Instance::Destroy(instance); };
    std::unique_ptr<Instance, decltype(deleter)> instance{Instance::Make(nullptr, 0, &memory), deleter};
    const auto& config = instance->GetAddressSpace()->GetConfig();
    const auto service = config.single_decode;
    REQUIRE(service.callback);
    REQUIRE(config.memory);
    for (const auto result : {CodeFetchResult::Success, CodeFetchResult::Fault, CodeFetchResult::Unsupported}) {
        CAPTURE(result);
        memory.fetch_result = result;
        const auto calls = memory.reads.size();
        ir::Block block{0, kPC};
        const auto end = block.GetEndLocation();
        const auto terminal = block.GetTerminal().which();
        const auto decoded = service.callback(service.context, kPC, *config.memory, block);
        REQUIRE(memory.reads.size() == calls + 1);
        REQUIRE(memory.reads.back().address == kPC);
        REQUIRE(memory.reads.back().size == 4);
        REQUIRE(memory.pointer_reads == 0);
        REQUIRE(memory.data_reads == 0);
        if (result == CodeFetchResult::Unsupported) {
            REQUIRE(decoded == SingleDecodeResult::Unsupported);
            REQUIRE(block.GetInstList().empty());
            REQUIRE(block.GetGuestCodeDependencies().empty());
            REQUIRE(block.GetEndLocation() == end);
            REQUIRE(block.GetTerminal().which() == terminal);
            continue;
        }
        REQUIRE(decoded == (result == CodeFetchResult::Success ? SingleDecodeResult::Ready : SingleDecodeResult::FaultIR));
        REQUIRE(block.HasTerminal());
        Machine machine;
        machine.Guest() = Initial();
        machine.state->host_cpu_flags = kOtherFlags | kNZCV;
        auto expected = Initial();
        const bool ready = result == CodeFetchResult::Success;
        if (ready) expected.r[0] = expected.r[1] + 5;
        else expected.padding = u32(Reason::PAGE_FATAL);
        backend::interp::Interpreter interpreter{*machine.state, &block};
        REQUIRE(interpreter.Run() == (ready ? HaltReason::None : HaltReason::CallHost));
        REQUIRE(std::memcmp(&machine.Guest(), &expected, sizeof(Context)) == 0);
        REQUIRE(machine.state->host_cpu_flags == (kOtherFlags | kNZCV));
        REQUIRE(machine.state->current_loc.Value() == kPC + (ready ? 4 : 0));
        REQUIRE(block.GetGuestCodeDependencies().size() == std::size_t(ready));
    }
    // FaultIR is a frontend trap category, not an unconditional PageFatal.
    ir::Block misaligned{0, kPC + 1};
    const auto calls = memory.reads.size();
    REQUIRE(service.callback(service.context, kPC + 1, *config.memory, misaligned) == SingleDecodeResult::FaultIR);
    REQUIRE(memory.reads.size() == calls);
    Machine machine;
    machine.Guest() = Initial();
    backend::interp::Interpreter interpreter{*machine.state, &misaligned};
    REQUIRE(interpreter.Run() == HaltReason::CallHost);
    REQUIRE(machine.Guest().padding == u32(Reason::ILL_CODE));
    REQUIRE(machine.state->current_loc.Value() == kPC + 1);
    REQUIRE(instance->GetAddressSpace()->GetConfig().single_decode.callback == service.callback);
}

TEST_CASE("ARM64 Core Step preserves context when its provider or engine is unavailable", "[arm64][single][step-boundary]") {
    using Instance = swift::translator::arm64::Arm64Instance;
    using Core = swift::translator::arm64::Arm64Core;
    using Exit = swift::translator::ExitReason;
    static_assert(static_cast<u32>(HaltReason::CallHost) == 0x100);
    static_assert(static_cast<u8>(Exit::Step) == 5);
    FetchMemory memory{0x91001420, kPC};
    memory.fetch_result = CodeFetchResult::Unsupported;
    auto destroy_instance = [](Instance* instance) { Instance::Destroy(instance); };
    auto destroy_core = [](Core* core) { Core::Destroy(core); };
    std::unique_ptr<Instance, decltype(destroy_instance)> instance{Instance::Make(nullptr, 0, &memory), destroy_instance};
    std::unique_ptr<Core, decltype(destroy_core)> core{Core::Make(instance.get()), destroy_core};
    core->GetContext() = Initial();
    core->GetContext().pc = kPC;
    const auto initial = core->GetContext();
    const auto syscall = core->GetSyscallNumber();
    const bool native = instance->GetAddressSpace()->GetConfig().enable_jit;
    for (unsigned retry = 0; retry < 3; ++retry) {
        REQUIRE(core->Step() == Exit::Unsupported);
        REQUIRE(std::memcmp(&core->GetContext(), &initial, sizeof(Context)) == 0);
        REQUIRE(core->GetSyscallNumber() == syscall);
        REQUIRE(memory.reads.size() == (retry + 1) * unsigned(native));
    }
    core->SignalInterrupt();
    REQUIRE(core->Step() == Exit::Signal);
    REQUIRE(memory.reads.size() == 3 * unsigned(native));
    REQUIRE(core->Step() == Exit::Signal);
    REQUIRE(std::memcmp(&core->GetContext(), &initial, sizeof(Context)) == 0);
    core->ClearInterrupt();
    REQUIRE(core->Step() == Exit::Unsupported);
    REQUIRE(std::memcmp(&core->GetContext(), &initial, sizeof(Context)) == 0);
    REQUIRE(memory.reads.size() == 4 * unsigned(native));
    for (const auto& read : memory.reads) {
        REQUIRE(read.address == kPC);
        REQUIRE(read.size == 4);
    }
    REQUIRE(memory.data_reads == 0);
    REQUIRE(memory.pointer_reads == 0);
}
