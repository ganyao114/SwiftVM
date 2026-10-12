#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/x64/single_instruction_code.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
constexpr VAddr kPc = 0x1000;
using Context = swift::arm64::ThreadContext64;

class Fetch final : public MemoryInterface {
public:
    CodeFetchResult ReadCode(void* output, size_t pc, size_t size) override {
        if (pc != kPc || size != sizeof(word)) return CodeFetchResult::Fault;
        std::memcpy(output, &word, sizeof(word));
        return CodeFetchResult::Success;
    }
    bool Read(void*, size_t, size_t) override { return false; }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void*) override { return nullptr; }
private:
    const u32 word = 0xF9400020; // LDR X0,[X1]
};

struct Program {
    ir::Block block{ir::Location{kPc}};
    Program() {
        Fetch fetch;
        ir::Assembler assembler{&block};
        swift::arm64::A64Decoder decoder{kPc, &fetch, &assembler};
        REQUIRE(decoder.DecodeSingle() == CodeFetchResult::Success);
        block.ReIdInstr();
    }
};

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
};

class Page final {
public:
    Page() {
        const long native = sysconf(_SC_PAGESIZE);
        REQUIRE(native > 0);
        size = static_cast<size_t>(native);
        address = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        REQUIRE(address != MAP_FAILED);
    }
    ~Page() { if (address != MAP_FAILED) munmap(address, size); }
    void* address{MAP_FAILED};
    size_t size{};
};

std::string MappingPermissions(std::uintptr_t address) {
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long begin{}, end{};
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%llx-%llx %4s", &begin, &end, permissions) == 3 &&
            address >= begin && address < end) return permissions;
    }
    return {};
}

struct Probe {
    const x64::SingleInstructionFaultDescriptor* descriptor{};
    backend::State* state{};
    x64::GuestAccessRange site{};
    size_t page_size{};
    u32 calls{};
    std::array<bool, 20> rejected{};
    bool admitted{}, last_byte{}, frame_matches{};
    u64 address{}, extent{};
};

bool ProbeRange(void* opaque, u64 address, u64 extent) {
    auto& probe = *static_cast<Probe*>(opaque);
    ++probe.calls;
    probe.address = address;
    probe.extent = extent;
    const auto& descriptor = *probe.descriptor;
    const auto frame = probe.state->spill_area[backend::kRiscvRecoveryFrameSlot];
    const auto recovery = probe.state->spill_area[backend::kRiscvRecoveryPcSlot];
    const auto site_pc = descriptor.RxBegin() + probe.site.begin_offset;
    const auto bias = reinterpret_cast<std::uintptr_t>(probe.state->pt);
    const auto host = bias + address;
    const auto classify = [&](std::uintptr_t pc, std::uintptr_t fault,
            std::uintptr_t recover, std::uintptr_t slot, const backend::State* state,
            std::uintptr_t r14, std::uintptr_t r13, std::uintptr_t rsp) {
        return descriptor.ClassifyFault(pc, fault, recover, slot, state, r14, r13, rsp);
    };
    // These are synthesized interrupted contexts checked while the real native
    // wrapper owns its frame. No real signal recovery is claimed by this test.
    probe.frame_matches = frame != 0 && descriptor.ExpectedFrame() == frame &&
                          recovery == descriptor.RecoveryPc();
    probe.admitted = classify(site_pc, host, recovery, frame, probe.state, host, frame, frame);
    probe.last_byte = classify(site_pc, host + extent - 1, recovery, frame,
                               probe.state, host, frame, frame);
    backend::State other{};
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    const std::array<bool, 20> accepted{
        classify(site_pc, host, recovery, frame, &other, host, frame, frame),
        classify(site_pc, host, recovery + 1, frame, probe.state, host, frame, frame),
        classify(site_pc, host, recovery, frame + 16, probe.state, host, frame, frame),
        classify(site_pc, host, recovery, frame, probe.state, host, frame + 16, frame),
        classify(site_pc, host, recovery, frame, probe.state, host, frame, frame - 16),
        classify(reinterpret_cast<std::uintptr_t>(&ProbeRange), host, recovery,
                 frame, probe.state, host, frame, frame),
        classify(descriptor.RxBegin(), host, recovery, frame, probe.state, host, frame, frame),
        classify(descriptor.RxEnd(), host, recovery, frame, probe.state, host, frame, frame),
        classify(descriptor.RxBegin() + probe.site.end_offset, host, recovery,
                 frame, probe.state, host, frame, frame),
        classify(site_pc, host - 1, recovery, frame, probe.state, host, frame, frame),
        classify(site_pc, host + extent, recovery, frame, probe.state, host, frame, frame),
        classify(site_pc, maximum, recovery, frame, probe.state, maximum - 3, frame, frame),
        classify(site_pc, bias - 1, recovery, frame, probe.state,
                 bias - 1, frame, frame),
        classify(site_pc, bias + probe.page_size - 4, recovery, frame, probe.state,
                 bias + probe.page_size - 4,
                 frame, frame),
        classify(site_pc, bias + 2 * probe.page_size - 4, recovery, frame, probe.state,
                 bias + 2 * probe.page_size - 4,
                 frame, frame),
        classify(site_pc, host, 0, frame, probe.state, host, frame, frame),
        classify(site_pc, host, recovery, 0, probe.state, host, frame, frame),
        classify(0, host, recovery, frame, probe.state, host, frame, frame),
        classify(site_pc, host, recovery, frame, nullptr, host, frame, frame),
        // A mutually consistent forged slot/R13/RSP is still rejected because
        // the bridge anchor is independent of the generated State slots.
        classify(site_pc, host, recovery, frame + 16, probe.state, host, frame + 16, frame + 16)
    };
    for (size_t index = 0; index < accepted.size(); ++index)
        probe.rejected[index] = !accepted[index];
    return true;
}
}

TEST_CASE("private single instruction owns RX lifetime and independent recovery frame", "[x64][step][private]") {
    // Exercise mask crossing with a looser limit, then limit crossing with a
    // looser mask. The fault address still falls inside each tested operand.
    const bool narrow_mask = GENERATE(false, true);
    CAPTURE(narrow_mask);
    Program program;
    x64::Translator translator{&program.block, {}, nullptr, nullptr, false,
                               x64::ExecutionMode::SingleInstruction};
    translator.Translate();
    REQUIRE(translator.GuestFaultCoverage() == x64::GuestAccessCoverage::Precise);
    REQUIRE(translator.GuestAccesses().size() == 1);
    Machine machine;
    Page page;
    constexpr u64 value = 0x9182736455AABBCCULL;
    std::memcpy(static_cast<u8*>(page.address) + 24, &value, sizeof(value));
    machine.Guest().r[0] = 0xDEADBEEF;
    machine.Guest().r[1] = 24;
    machine.state->pt = page.address;
    machine.state->guest_addr_limit = narrow_mask ? 2 * page.size : page.size;
    machine.state->guest_addr_mask = (narrow_mask ? page.size : 2 * page.size) - 1;
    Probe probe;
    probe.state = machine.state;
    probe.site = translator.GuestAccesses().front();
    probe.page_size = page.size;
    machine.state->interp_range_check = &ProbeRange;
    machine.state->interp_range_check_ctx = &probe;
    const auto readers = backend::riscv64::anonymous_memory_readers.load();
    std::uintptr_t rx{};
    {
        x64::PrivateSingleInstructionCode code{machine.state,
                {translator.getCode(), translator.getSize()}, translator.RecoveryOffset(),
                translator.FrameSize(), kPc, translator.GuestAccesses(), translator.GuestFaultCoverage()};
        const auto& descriptor = code.Descriptor();
        probe.descriptor = &descriptor;
        rx = descriptor.RxBegin();
        REQUIRE(descriptor.RxEnd() - rx == translator.getSize());
        REQUIRE(descriptor.OriginalPc() == kPc);
        REQUIRE(descriptor.ExpectedFrame() == 0);
        REQUIRE(MappingPermissions(rx) == "r-xp");
        REQUIRE_FALSE(descriptor.ClassifyFault(rx + probe.site.begin_offset,
                reinterpret_cast<std::uintptr_t>(page.address) + 24,
                descriptor.RecoveryPc(), 0, machine.state,
                reinterpret_cast<std::uintptr_t>(page.address) + 24, 0, 0));
        REQUIRE(code.Run() == HaltReason::Step);
        REQUIRE(probe.calls == 1);
        REQUIRE(probe.address == 24);
        REQUIRE(probe.extent == 8);
        REQUIRE(probe.frame_matches);
        REQUIRE(probe.admitted);
        REQUIRE(probe.last_byte);
        for (size_t index = 0; index < probe.rejected.size(); ++index) {
            CAPTURE(index);
            REQUIRE(probe.rejected[index]);
        }
        REQUIRE(machine.Guest().r[0] == value);
        REQUIRE(machine.state->current_loc.Value() == kPc + 4);
        REQUIRE(descriptor.ExpectedFrame() == 0);
        REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
        REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
        REQUIRE(machine.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
        REQUIRE(backend::riscv64::anonymous_memory_readers.load() == readers);
        REQUIRE_FALSE(descriptor.ClassifyFault(rx + probe.site.begin_offset,
                reinterpret_cast<std::uintptr_t>(page.address) + 24,
                descriptor.RecoveryPc(), 0, machine.state,
                reinterpret_cast<std::uintptr_t>(page.address) + 24, 0, 0));
    }
    // No intervening allocation can reuse the released address before this
    // kernel query. mincore neither dereferences nor recreates the mapping.
    unsigned char resident{};
    errno = 0;
    const int result = mincore(reinterpret_cast<void*>(rx), page.size, &resident);
    const int error = errno;
    REQUIRE(result == -1);
    REQUIRE(error == ENOMEM);
}

TEST_CASE("private single instruction rejects unsupported or malformed metadata", "[x64][step][private]") {
    Program program;
    x64::Translator translator{&program.block, {}, nullptr, nullptr, false,
                               x64::ExecutionMode::SingleInstruction};
    translator.Translate();
    Machine machine;
    const auto bytes = std::span<const u8>{translator.getCode(), translator.getSize()};
    const auto frame = translator.FrameSize();
    const auto recovery = translator.RecoveryOffset();
    const auto valid = translator.GuestAccesses().front();
    const auto precise = x64::GuestAccessCoverage::Precise;
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, bytes, recovery,
            frame, kPc, translator.GuestAccesses(), x64::GuestAccessCoverage::Unsupported}),
            std::invalid_argument);
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{nullptr, bytes, recovery,
            frame, kPc, translator.GuestAccesses(), precise}), std::invalid_argument);
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, {}, 0,
            frame, kPc, {}, precise}), std::invalid_argument);
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, bytes, u32(bytes.size()),
            frame, kPc, translator.GuestAccesses(), precise}), std::invalid_argument);
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, bytes, recovery,
            frame + 1, kPc, translator.GuestAccesses(), precise}), std::invalid_argument);
    const std::array<x64::GuestAccessRange, 5> malformed{
        x64::GuestAccessRange{valid.begin_offset, valid.begin_offset, 8, valid.kind},
        x64::GuestAccessRange{valid.begin_offset, recovery + 1, 8, valid.kind},
        x64::GuestAccessRange{valid.begin_offset, valid.end_offset, 3, valid.kind},
        x64::GuestAccessRange{valid.begin_offset, valid.end_offset, 8, static_cast<x64::GuestAccessKind>(255)},
        x64::GuestAccessRange{valid.end_offset, valid.begin_offset, 8, valid.kind}
    };
    for (const auto& site : malformed) {
        REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, bytes, recovery,
                frame, kPc, std::span{&site, 1}, precise}), std::invalid_argument);
    }
    const std::array<x64::GuestAccessRange, 2> overlap{valid, valid};
    REQUIRE_THROWS_AS((x64::PrivateSingleInstructionCode{machine.state, bytes, recovery,
            frame, kPc, overlap, precise}), std::invalid_argument);
    REQUIRE(machine.state->halt_reason == HaltReason::None);
    REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
    REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
}
