#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <xmmintrin.h>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/common/unsigned_estimate.h"

using namespace swift;
using namespace swift::runtime;
using Clock = std::chrono::steady_clock;

namespace {
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 256> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    std::array<u64, 64> memory{};
    Storage() {
        state->pt = memory.data(); state->guest_addr_limit = sizeof(memory);
        for (u32 i = 0; i < 256; ++i) state->uniform_buffer_begin[i] = static_cast<u8>(i * 17 + 3);
    }
};

IntrusivePtr<ir::Block> Program(const std::string& name) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x1000})};
    ir::Assembler as{block.get()};
    if (name == "vector") {
        auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
        auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
        for (u32 i = 0; i < 64; ++i) {
            value = as.VecAdd(value, other, ir::Imm{u32{32}}).SetType(ValueType::V128);
            value = as.VecXor(value, other).SetType(ValueType::V128);
        }
        as.StoreUniform(ir::Uniform{32, ValueType::V128}, value);
    } else if (name == "memory") {
        for (u32 i = 0; i < 32; ++i) {
            auto address = as.LoadImm(ir::Imm{u64{i * 8}}).SetType(ValueType::U64);
            auto value = as.LoadMemory(ir::Operand{address}).SetType(ValueType::U64);
            value = as.Add(value, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
            as.StoreMemory(ir::Operand{address}, value);
        }
    } else {
        auto value = as.LoadUniform(ir::Uniform{0, ValueType::U64}).SetType(ValueType::U64);
        for (u32 i = 0; i < 128; ++i) {
            value = as.Add(value, ir::Operand{ir::Imm{u64{17}}}).SetType(ValueType::U64);
            if (name == "flags" || name == "branch") as.SaveFlags(value, ir::Flags::NZCV);
            if (name == "branch") {
                auto condition = as.CondSet(ir::Cond(i % 14)).SetType(ValueType::U8);
                value = as.Select(ir::BOOL{condition}, value, as.LoadImm(ir::Imm{u64{42}}).SetType(ValueType::U64)).SetType(ValueType::U64);
            } else value = as.Xor(value, ir::Operand{ir::Imm{u64{0x51237}}}).SetType(ValueType::U64);
        }
        as.StoreUniform(ir::Uniform{32, ValueType::U64}, value);
    }
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> CryptoProgram(const std::string& name) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2000})};
    ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    if (name == "pclmul") {
        constexpr std::array<u32, 4> controls{0, 1, 16, 17};
        for (u32 i = 0; i < 64; ++i) {
            value = as.VecPclMul(value, other, ir::Imm{controls[i % 4]}).SetType(ValueType::V128);
            value = as.VecXor(value, other).SetType(ValueType::V128);
        }
    } else {
        auto zero = as.VecSharedZero().SetType(ValueType::V128);
        for (u32 i = 0; i < 64; ++i) {
            if (name == "aes_enc") {
                value = i == 63 ? as.VecAesEncLastFast(value, other, zero).SetType(ValueType::V128) :
                                  as.VecAesEncFast(value, other, zero).SetType(ValueType::V128);
            } else if (name == "aes_dec") {
                value = i == 63 ? as.VecAesDecLastFast(value, other, zero).SetType(ValueType::V128) :
                                  as.VecAesDecFast(value, other, zero).SetType(ValueType::V128);
            } else throw std::runtime_error("unknown crypto benchmark");
        }
    }
    as.StoreUniform(ir::Uniform{32, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> DotProgram(bool shared, bool sign) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3000})};
    ir::Assembler as{block.get()};
    auto lhs = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto rhs = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto value = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    std::array<ir::Value, 2> left, right;
    for (u32 half = 0; !shared && half < 2; ++half) {
        left[half] = as.VecExtract64(lhs, ir::Imm{half}).SetType(ValueType::U64);
        right[half] = as.VecExtract64(rhs, ir::Imm{half}).SetType(ValueType::U64);
    }
    const auto imm = [&](u64 v) { return as.LoadImm(ir::Imm{v}).SetType(ValueType::U64); };
    const auto byte = [&](const std::array<ir::Value, 2>& source, u32 position) {
        auto v = as.LsrImm(source[position / 8], ir::Imm{position % 8 * 8}).SetType(ValueType::U64);
        v = as.And(v, ir::Operand{ir::Imm{u64{255}}}).SetType(ValueType::U64);
        if (sign) {
            v = as.LslImm(v, ir::Imm{u32{56}}).SetType(ValueType::U64);
            v = as.AsrImm(v, ir::Imm{u32{56}}).SetType(ValueType::U64);
        }
        return v;
    };
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        if (shared) {
            value = as.VecDot(lhs, rhs, value, ir::Imm{u32(sign)}).SetType(ValueType::V128);
            continue;
        }
        std::array<ir::Value, 2> old, result{imm(0), imm(0)};
        for (u32 half = 0; half < 2; ++half) old[half] = as.VecExtract64(value, ir::Imm{half}).SetType(ValueType::U64);
        for (u32 lane = 0; lane < 4; ++lane) {
            auto sum = as.LsrImm(old[lane / 2], ir::Imm{lane % 2 * 32}).SetType(ValueType::U64);
            for (u32 element = 0; element < 4; ++element) {
                auto product = as.Mul(byte(left, lane * 4 + element), ir::Operand{byte(right, lane * 4 + element)})
                                  .SetType(ValueType::U64);
                sum = as.Add(sum, ir::Operand{product}).SetType(ValueType::U64);
            }
            sum = as.And(sum, ir::Operand{ir::Imm{u64{0xFFFFFFFF}}}).SetType(ValueType::U64);
            sum = as.LslImm(sum, ir::Imm{lane % 2 * 32}).SetType(ValueType::U64);
            result[lane / 2] = as.Or(result[lane / 2], ir::Operand{sum}).SetType(ValueType::U64);
        }
        auto low = as.VecDup64(result[0]).SetType(ValueType::V128);
        auto high = as.VecDup64(result[1]).SetType(ValueType::V128);
        value = as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> SaturatingMultiplyProgram(bool shared, u32 bits, u32 mode) {
    using ir::ValueType;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x4000})};
    ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, ValueType::V128}).SetType(ValueType::V128);
    auto other = as.LoadUniform(ir::Uniform{16, ValueType::V128}).SetType(ValueType::V128);
    auto acc = as.LoadUniform(ir::Uniform{32, ValueType::V128}).SetType(ValueType::V128);
    const auto imm = [&](u64 v) { return as.LoadImm(ir::Imm{v}).SetType(ValueType::U64); };
    auto qc = imm(0);
    const auto lane = [&](const std::array<ir::Value, 2>& source, u32 index) {
        auto v = as.LsrImm(source[index * bits / 64], ir::Imm{index * bits % 64}).SetType(ValueType::U64);
        v = as.LslImm(v, ir::Imm{64 - bits}).SetType(ValueType::U64);
        return as.AsrImm(v, ir::Imm{64 - bits}).SetType(ValueType::U64);
    };
    for (u32 iteration = 0; iteration < 64; ++iteration) {
        if (shared) {
            value = as.VecSatMulHigh(value, other, acc, ir::Imm{bits},
                                     ir::Imm{((128 / bits) << 2) | mode}).SetType(ValueType::V128);
            auto saturated = as.VecSatMulQC(value).SetType(ValueType::U8);
            saturated = as.ZeroExtend64(saturated).SetType(ValueType::U64);
            qc = as.Or(qc, ir::Operand{saturated}).SetType(ValueType::U64);
            continue;
        }
        std::array<ir::Value, 2> left, right, old, output{imm(0), imm(0)};
        for (u32 half = 0; half < 2; ++half) {
            left[half] = as.VecExtract64(value, ir::Imm{half}).SetType(ValueType::U64);
            right[half] = as.VecExtract64(other, ir::Imm{half}).SetType(ValueType::U64);
            old[half] = as.VecExtract64(acc, ir::Imm{half}).SetType(ValueType::U64);
        }
        const auto lower = imm(u64(-(s64{1} << (bits - 1))));
        const auto upper = imm((u64{1} << (bits - 1)) - 1);
        for (u32 index = 0; index < 128 / bits; ++index) {
            auto exact = as.Mul(lane(left, index), ir::Operand{lane(right, index)}).SetType(ValueType::U64);
            if (mode == 3) exact = as.Sub(imm(0), ir::Operand{exact}).SetType(ValueType::U64);
            if (mode) exact = as.Add(exact, ir::Operand{ir::Imm{u64{1} << (bits - 2)}}).SetType(ValueType::U64);
            exact = as.AsrImm(exact, ir::Imm{bits - 1}).SetType(ValueType::U64);
            if (mode >= 2) exact = as.Add(exact, ir::Operand{lane(old, index)}).SetType(ValueType::U64);
            // The extracted product plus accumulator fits signed 34 bits.
            // Difference sign masks therefore implement exact comparisons
            // without changing the virtual flags or relying on another IR.
            auto low_mask = as.Sub(exact, ir::Operand{lower}).SetType(ValueType::U64);
            low_mask = as.AsrImm(low_mask, ir::Imm{u32{63}}).SetType(ValueType::U64);
            auto high_mask = as.Sub(upper, ir::Operand{exact}).SetType(ValueType::U64);
            high_mask = as.AsrImm(high_mask, ir::Imm{u32{63}}).SetType(ValueType::U64);
            auto both = as.Or(low_mask, ir::Operand{high_mask}).SetType(ValueType::U64);
            auto changed = as.And(both, ir::Operand{ir::Imm{u64{1}}}).SetType(ValueType::U64);
            qc = as.Or(qc, ir::Operand{changed}).SetType(ValueType::U64);
            auto selected = as.And(lower, ir::Operand{low_mask}).SetType(ValueType::U64);
            auto retained = as.AndNot(exact, ir::Operand{low_mask}).SetType(ValueType::U64);
            exact = as.Or(selected, ir::Operand{retained}).SetType(ValueType::U64);
            selected = as.And(upper, ir::Operand{high_mask}).SetType(ValueType::U64);
            retained = as.AndNot(exact, ir::Operand{high_mask}).SetType(ValueType::U64);
            exact = as.Or(selected, ir::Operand{retained}).SetType(ValueType::U64);
            exact = as.And(exact, ir::Operand{ir::Imm{(u64{1} << bits) - 1}}).SetType(ValueType::U64);
            exact = as.LslImm(exact, ir::Imm{index * bits % 64}).SetType(ValueType::U64);
            output[index * bits / 64] = as.Or(output[index * bits / 64], ir::Operand{exact}).SetType(ValueType::U64);
        }
        auto low = as.VecDup64(output[0]).SetType(ValueType::V128);
        auto high = as.VecDup64(output[1]).SetType(ValueType::V128);
        value = as.VecZip(low, high, ir::Imm{u32{64}}, ir::Imm{u32{0}}).SetType(ValueType::V128);
    }
    as.StoreUniform(ir::Uniform{64, ValueType::V128}, value);
    as.StoreUniform(ir::Uniform{80, ValueType::U64}, qc);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

double Median(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end()); return samples[samples.size() / 2];
}

template<typename Run> double Measure(Storage& storage, u32 iterations, Run run) {
    for (u32 i = 0; i < 1000; ++i) { storage.state->halt_reason = HaltReason::None; run(); }
    std::vector<double> samples;
    for (u32 repeat = 0; repeat < 7; ++repeat) {
        const auto begin = Clock::now();
        for (u32 i = 0; i < iterations; ++i) {
            storage.state->halt_reason = HaltReason::None;
            if (run() != HaltReason::CallHost) throw std::runtime_error("benchmark block did not return to host");
        }
        samples.push_back(std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / iterations);
    }
    return Median(samples);
}

void MeasureCrypto(u32 iterations) {
    using backend::x64::HostFeatures;
    const auto features = HostFeatures::Detect();
    std::cerr << "crypto feature contract: auto detects sse41=" << features.sse41 << ",fma=" << features.fma
              << ",aes=" << features.aes << ",pclmul=" << features.pclmul << ",sse42=" << features.sse42 << ",sha=" << features.sha
              << "; baseline requests all extensions disabled; unsupported extensions cannot be enabled\n";
    std::cout << "crypto_workload,route,ir_instructions,code_bytes,frame_bytes,compile_us,execution_ns,baseline_over_route\n";
    for (const std::string name : {"aes_enc", "aes_dec", "pclmul"}) {
        auto block = CryptoProgram(name);
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{};
        u32 instructions{}; for (auto& inst : block->GetInstList()) { (void)inst; ++instructions; }
        for (u32 route = 0; route < 2; ++route) {
            const auto requested = route ? HostFeatures{} : features;
            std::vector<double> compile_samples;
            for (u32 i = 0; i < 25; ++i) {
                const auto begin = Clock::now(); backend::x64::Translator candidate{block.get(), requested}; candidate.Translate();
                compile_samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), requested}; translated.Translate();
            auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) || actual.memory != expected.memory)
                throw std::runtime_error(name + " crypto benchmark native/interpreter result mismatch");
            compile_us[route] = Median(compile_samples); code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << name << ',' << (route ? "baseline" : "auto") << ',' << instructions << ',' << code_bytes[route] << ','
                      << frame_bytes[route] << ',' << std::fixed << std::setprecision(2) << compile_us[route] << ','
                      << execution_ns[route] << ',' << execution_ns[1] / execution_ns[route] << '\n';
    }
}

void MeasureDot(u32 iterations) {
    std::cout << "dot_workload,route,ir_instructions,code_bytes,frame_bytes,compile_us,execution_ns,expanded_over_route\n";
    for (bool sign : {false, true}) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        std::array<u8, 16> expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = DotProgram(route != 0, sign);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now();
                backend::x64::Translator candidate{block.get()}; candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get()}; translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256))
                throw std::runtime_error("dot benchmark native/interpreter mismatch");
            if (route == 0) std::memcpy(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16);
            else if (std::memcmp(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 16))
                throw std::runtime_error("dot benchmark shared/expanded mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples);
            code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << (sign ? "sdot" : "udot") << ',' << (route ? "shared_sse2" : "expanded_scalar") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ','
                      << std::fixed << std::setprecision(2) << compile_us[route] << ',' << execution_ns[route] << ','
                      << execution_ns[0] / execution_ns[route] << '\n';
    }
}

void MeasureSaturatingMultiply(u32 iterations) {
    constexpr std::array<const char*, 4> names{"sqdmulh", "sqrdmulh", "sqrdmlah", "sqrdmlsh"};
    std::cout << "satmul_workload,route,ir_instructions,code_bytes,frame_bytes,compile_us,execution_ns,expanded_over_route\n";
    for (u32 bits : {16u, 32u}) for (u32 mode = 0; mode < 4; ++mode) {
        std::array<double, 2> compile_us{}, execution_ns{};
        std::array<u32, 2> code_bytes{}, frame_bytes{}, instructions{};
        std::array<u8, 24> expanded_result{};
        for (u32 route = 0; route < 2; ++route) {
            auto block = SaturatingMultiplyProgram(route != 0, bits, mode);
            std::vector<double> samples;
            for (u32 repeat = 0; repeat < 25; ++repeat) {
                const auto begin = Clock::now();
                backend::x64::Translator candidate{block.get(), backend::x64::HostFeatures{}};
                candidate.Translate();
                samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
            }
            backend::x64::Translator translated{block.get(), backend::x64::HostFeatures{}};
            translated.Translate();
            const auto native = translated.getCode<HaltReason (*)(backend::State*)>();
            Storage actual, expected;
            backend::interp::Interpreter reference{*expected.state, block.get()};
            if (native(actual.state) != HaltReason::CallHost || reference.Run() != HaltReason::CallHost ||
                std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256))
                throw std::runtime_error("saturating multiply benchmark native/interpreter mismatch");
            if (!route) std::memcpy(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 24);
            else if (std::memcmp(expanded_result.data(), actual.state->uniform_buffer_begin + 64, 24))
                throw std::runtime_error("saturating multiply benchmark shared/expanded result or QC mismatch");
            for (auto& inst : block->GetInstList()) { (void)inst; ++instructions[route]; }
            compile_us[route] = Median(samples);
            code_bytes[route] = translated.getSize(); frame_bytes[route] = translated.FrameSize();
            execution_ns[route] = Measure(actual, iterations, [&] { return native(actual.state); });
        }
        for (u32 route = 0; route < 2; ++route)
            std::cout << names[mode] << '_' << bits << ',' << (route ? "shared_native" : "expanded_scalar") << ','
                      << instructions[route] << ',' << code_bytes[route] << ',' << frame_bytes[route] << ','
                      << std::fixed << std::setprecision(2) << compile_us[route] << ',' << execution_ns[route] << ','
                      << execution_ns[0] / execution_ns[route] << '\n';
    }
}

#include "crc_ieee_benchmark.inc"
#include "satshift_benchmark.inc"
#include "compare64_benchmark.inc"
#include "mulpacked_benchmark.inc"
#include "unsigned_estimate_benchmark.inc"
#include "sataddsub_benchmark.inc"
}

int main(int argc, char** argv) {
    const u32 iterations = argc > 1 ? std::stoul(argv[1]) : 10000;
    if (!iterations) return 1;
    const std::string mode = argc > 2 ? argv[2] : "";
    if (argc > 3 || (!mode.empty() && mode != "--crypto" && mode != "--crypto-only" && mode != "--dot-only" && mode != "--satmul-only" && mode != "--crc-only" && mode != "--satshift-only" && mode != "--compare64-only" && mode != "--mulpacked-only" && mode != "--unsigned-estimate-only" && mode != "--sataddsub-only"))
        throw std::runtime_error("usage: swift_x64_benchmark [iterations] [--crypto|--crypto-only|--dot-only|--satmul-only|--crc-only|--satshift-only|--compare64-only|--mulpacked-only|--unsigned-estimate-only|--sataddsub-only]");
    if (mode == "--crypto-only") { MeasureCrypto(iterations); return 0; }
    if (mode == "--dot-only") { MeasureDot(iterations); return 0; }
    if (mode == "--satmul-only") { MeasureSaturatingMultiply(iterations); return 0; }
    if (mode == "--crc-only") { MeasureIeeeCrc(iterations); return 0; }
    if (mode == "--satshift-only") { MeasureSaturatingShift(iterations); return 0; }
    if (mode == "--compare64-only") { MeasureCompare64(iterations); return 0; }
    if (mode == "--mulpacked-only") { MeasureMulPacked(iterations); return 0; }
    if (mode == "--unsigned-estimate-only") { MeasureUnsignedEstimate(iterations); return 0; }
    if (mode == "--sataddsub-only") { MeasureSatAddSub(iterations); return 0; }
    std::cout << "workload,ir_instructions,code_bytes,frame_bytes,compile_us,native_ns,interpreter_ns,speedup\n";
    for (const std::string name : {"scalar", "flags", "branch", "vector", "memory"}) {
        auto block = Program(name);
        std::vector<double> compile;
        for (u32 i = 0; i < 25; ++i) {
            const auto begin = Clock::now(); backend::x64::Translator candidate{block.get()}; candidate.Translate();
            compile.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count());
        }
        backend::x64::Translator translated{block.get()}; translated.Translate();
        auto native = translated.getCode<HaltReason (*)(backend::State*)>();
        Storage actual, expected;
        backend::interp::Interpreter reference{*expected.state, block.get()};
        native(actual.state); reference.Run();
        if (std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 256) || actual.memory != expected.memory)
            throw std::runtime_error(name + " benchmark result mismatch");
        const auto native_ns = Measure(actual, iterations, [&] { return native(actual.state); });
        const auto interpreter_ns = Measure(expected, iterations, [&] { return reference.Run(); });
        u32 instructions{}; for (auto& inst : block->GetInstList()) { (void)inst; ++instructions; }
        std::cout << name << ',' << instructions << ',' << translated.getSize() << ',' << translated.FrameSize() << ','
                  << std::fixed << std::setprecision(2) << Median(compile) << ',' << native_ns << ',' << interpreter_ns << ',' << interpreter_ns / native_ns << '\n';
    }
    if (mode == "--crypto") MeasureCrypto(iterations);
}
