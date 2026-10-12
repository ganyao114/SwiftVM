#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/x64/uniform_forward.h"
#include "runtime/backend/x64/flags_liveness.h"
#include "runtime/common/svm_config.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using Context = swift::arm64::ThreadContext64;
using Clock = std::chrono::steady_clock;
using Fn = HaltReason (*)(backend::State*);

namespace {
constexpr u64 code_base = 0x1000;
struct Memory final : MemoryInterface {
    std::vector<u32> words;
    bool Read(void* dest, size_t address, size_t size) override {
        if (address < code_base || address - code_base > words.size() * 4 ||
            size > words.size() * 4 - (address - code_base)) return false;
        std::memcpy(dest, reinterpret_cast<const u8*>(words.data()) + address - code_base, size);
        return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        const auto pos = reinterpret_cast<uintptr_t>(address);
        if (pos < code_base || pos - code_base >= words.size() * 4) return nullptr;
        return reinterpret_cast<u8*>(words.data()) + pos - code_base;
    }
};
struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context)> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
    void Init(u64 seed) {
        Guest() = {};
        for (u32 i = 0; i < 29; ++i) Guest().r[i] = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
        Guest().fp = seed; Guest().lr = ~seed; Guest().sp = 0x8000;
        state->host_cpu_flags = seed & 0xf0000000;
        state->halt_reason = HaltReason::None;
    }
};
std::unique_ptr<ir::Block> Decode(Memory& memory) {
    auto block = std::make_unique<ir::Block>(0, code_base);
    ir::Assembler as{block.get()};
    swift::arm64::A64Decoder decoder{code_base, &memory, &as}; decoder.Decode();
    return block;
}
Memory Program(const std::string& name) {
    Memory memory;
    for (u32 i = 0; i < 96; ++i) {
        if (name == "wx_chain") {
            // ADD W0,W0,#1 alternates with ADD X0,X0,#1.
            memory.words.push_back((i & 1 ? 0x91000000u : 0x11000000u) | (1u << 10));
        } else if (name == "pressure") {
            const u32 dst = i % 29, src = (i + 7) % 29;
            memory.words.push_back(0x8b000000u | (src << 16) | (dst << 5) | dst);
        } else {
            const u32 dst = i % 8, src = (i + 1) % 8;
            // Alternating ADDS / ADCS gives host-flags clobber pressure.
            memory.words.push_back((i & 1 ? 0xba000000u : 0xab000000u) |
                                   (src << 16) | (dst << 5) | dst);
        }
    }
    memory.words.push_back(0xd4200000u);
    return memory;
}
double Median(std::vector<double> data) {
    std::sort(data.begin(), data.end()); return data[data.size() / 2];
}
void Prepare(ir::Block* block, bool forward, bool flags_liveness) {
    if (forward) x64::ForwardUniformLoads(block, sizeof(Context));
    ir::ConstFoldingPass::Run(block, GetSvmConfig().GetFeatureSet());
    ir::DeadCodeEliminationPass::Run(block);
    if (flags_liveness) x64::NarrowCarryChainFlags(block, sizeof(Context));
    block->ReIdInstr();
}
void Validate(Memory& memory, ir::Block* block, Fn fn) {
    auto reference = Decode(memory);
    for (u64 seed : {u64{0}, UINT64_MAX, u64{0x123456789abcdef0}, u64{0xf0000000}}) {
        Machine expected, actual; expected.Init(seed); actual.Init(seed);
        backend::interp::Interpreter interpreter{*expected.state, reference.get()}; interpreter.Run();
        const auto result = fn(actual.state);
        if (result != expected.state->halt_reason ||
            std::memcmp(&actual.Guest(), &expected.Guest(), sizeof(Context)) ||
            actual.state->host_cpu_flags != expected.state->host_cpu_flags ||
            actual.state->current_loc != expected.state->current_loc)
            throw std::runtime_error("A64 workload full-state mismatch");
        if (memory.words.front() == 0x11000400u) {
            u64 value = seed ^ 0x9e3779b97f4a7c15ULL;
            for (u32 i = 0; i < 96; ++i) value = i & 1 ? value + 1 : u32(value + 1);
            if (actual.Guest().r[0] != value)
                throw std::runtime_error("A64 W/X chain independent arithmetic mismatch");
        } else {
            std::array<u64, 29> registers;
            for (u32 i = 0; i < 29; ++i) registers[i] = seed ^ (0x9e3779b97f4a7c15ULL * (i + 1));
            u64 flags = seed & 0xf0000000;
            const bool pressure = memory.words.front() == (0x8b000000u | (7u << 16));
            for (u32 i = 0; i < 96; ++i) {
                const u32 dst = i % (pressure ? 29 : 8), src = (i + (pressure ? 7 : 1)) % (pressure ? 29 : 8);
                const u64 a = registers[dst], b = registers[src];
                const u64 carry = !pressure && (i & 1) ? (flags >> 29) & 1 : 0;
                const unsigned __int128 sum = static_cast<unsigned __int128>(a) + b + carry;
                const u64 result = static_cast<u64>(sum); registers[dst] = result;
                if (!pressure) {
                    const u64 overflow = ((~(a ^ b) & (a ^ result)) >> 63) & 1;
                    flags = ((result >> 63) << 31) | (u64(result == 0) << 30) |
                            (u64(sum >> 64) << 29) | (overflow << 28);
                }
            }
            if (actual.Guest().r != registers || actual.state->host_cpu_flags != flags)
                throw std::runtime_error("A64 pressure/carry independent arithmetic mismatch");
        }
    }
    (void)block;
}
std::array<double, 8> Measure(const std::array<Fn, 8>& fn, u32 iterations) {
    std::array<Machine, 8> machines;
    std::array<std::vector<double>, 8> samples;
    for (u32 route = 0; route < 8; ++route) {
        machines[route].Init(0x123456789abcdef0);
        for (u32 i = 0; i < 1000; ++i) {
            machines[route].state->halt_reason = HaltReason::None; fn[route](machines[route].state);
        }
    }
    for (u32 repeat = 0; repeat < 7; ++repeat) {
        for (u32 order = 0; order < 8; ++order) {
            const u32 route = (order + repeat) & 7;
            auto& machine = machines[route];
            const auto begin = Clock::now();
            for (u32 i = 0; i < iterations; ++i) {
                machine.state->halt_reason = HaltReason::None;
                if (fn[route](machine.state) != HaltReason::CallHost) throw std::runtime_error("A64 workload exit mismatch");
            }
            samples[route].push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / iterations);
        }
    }
    std::array<double, 8> medians;
    for (u32 route = 0; route < 8; ++route) medians[route] = Median(samples[route]);
    return medians;
}
}
int main(int argc, char** argv) {
    const u32 iterations = argc > 1 ? std::stoul(argv[1]) : 20000;
    if (!iterations || argc > 2) throw std::runtime_error("usage: swift_x64_uniform_benchmark [iterations]");
    std::cout << "a64_workload,forward,scalar_cache,flags_liveness,uniform_loads,uniform_stores,observed_ssa_load_sites,observed_ssa_store_sites,scalar_cache_read_sites,scalar_cache_write_sites,coalesced_binary_sites,flags_snapshot_sites,flags_materialization_ir_sites,helper_call_sites,code_bytes,frame_bytes,decode_prepare_emit_us,native_ns\n";
    for (const std::string name : {"wx_chain", "pressure", "carry_chain"}) {
        auto memory = Program(name);
        std::array<std::vector<double>, 8> compile;
        for (u32 i = 0; i < 25; ++i) {
            for (u32 order = 0; order < 8; ++order) {
                const u32 route = (order + i) & 7;
                const auto begin = Clock::now(); auto sample = Decode(memory); Prepare(sample.get(), route & 1, route & 4);
                x64::Translator translator{sample.get(), x64::HostFeatures::Detect(), nullptr, nullptr, bool(route & 2)}; translator.Translate();
                compile[route].push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
        }
        std::array<std::unique_ptr<ir::Block>, 8> blocks;
        std::array<std::unique_ptr<x64::Translator>, 8> translators;
        std::array<Fn, 8> functions;
        for (u32 route = 0; route < 8; ++route) {
            blocks[route] = Decode(memory); Prepare(blocks[route].get(), route & 1, route & 4);
            translators[route] = std::make_unique<x64::Translator>(blocks[route].get(), x64::HostFeatures::Detect(), nullptr, nullptr, bool(route & 2));
            translators[route]->Translate(); functions[route] = translators[route]->getCode<Fn>();
            Validate(memory, blocks[route].get(), functions[route]);
        }
        const auto ns = Measure(functions, iterations);
        for (u32 route = 0; route < 8; ++route) {
        auto& block = blocks[route]; auto& translator = *translators[route];
        u32 loads{}, stores{};
        for (auto& inst : block->GetInstList()) {
            loads += inst.GetOp() == ir::OpCode::LoadUniform;
            stores += inst.GetOp() == ir::OpCode::StoreUniform;
        }
        const auto& stats = translator.Statistics();
        const auto& ssa = stats.At(x64::TransferRegion::CanonicalSSA);
        std::cout << std::fixed << std::setprecision(2) << name << ',' << (route & 1) << ',' << bool(route & 2) << ',' << bool(route & 4) << ',' << loads << ',' << stores << ','
                  << ssa.scalar_load_sites + ssa.vector_load_sites << ','
                  << ssa.scalar_store_sites + ssa.vector_store_sites << ','
                  << stats.scalar_cache_read_sites << ',' << stats.scalar_cache_write_sites << ','
                  << stats.scalar_coalesced_binary_sites << ','
                  << stats.flags_snapshot_sites << ',' << stats.flags_materialization_ir_sites << ','
                  << stats.helper_call_sites << ','
                  << translator.getSize() << ',' << translator.FrameSize() << ',' << Median(compile[route]) << ',' << ns[route] << '\n';
        }
    }
}
