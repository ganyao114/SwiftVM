#include <biscuit/assembler.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace biscuit;
using Word = std::uint64_t;
using Memory = std::array<Word, 4>;
constexpr Memory kMemory{0x8877665544332211, 0x123456789abcdef0,
                         0xfedcba9876543210, 0x55aa55aa55aa55aa};

struct Case {
    std::string name;
    std::string function;
    Word left{};
    Word right{};
    Memory memory{kMemory};
    Word result{};
    Memory expected_memory{kMemory};
};

struct Function {
    std::vector<std::uint8_t> bytes;
    Word cookie;
};

std::string Hex(Word value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

void WriteMemory(std::ostream& out, const Memory& memory) {
    out << '[';
    for (std::size_t i = 0; i < memory.size(); ++i) {
        if (i) out << ',';
        out << '"' << Hex(memory[i]) << '"';
    }
    out << ']';
}

void EmitBundle(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    if (!std::filesystem::is_empty(directory)) {
        throw std::runtime_error("Output directory must be empty");
    }
    std::map<std::string, Function> functions;
    std::vector<Case> cases;
    const auto emit = [&](const std::string& name, const auto& body) {
        std::array<std::uint8_t, 1024> bytes{};
        Assembler as(bytes.data(), bytes.size(), ArchFeature::RV64);
        as.DisableOptimization(Optimization::AutoCompress);
        const Word cookie = functions.size() + 1;
        // Each invocation must prove that it entered this generated function.
        // t6 is a caller-saved register reserved by this fixture contract.
        as.LI(t6, cookie);
        body(as);
        const auto size = static_cast<std::size_t>(as.GetCodeBuffer().GetCursorOffset());
        if (size == 0 || size % 4 != 0 || size > bytes.size()) {
            throw std::runtime_error("Invalid RV64G function size: " + name);
        }
        if (!functions.emplace(name, Function{std::vector(bytes.begin(), bytes.begin() + size), cookie}).second) {
            throw std::runtime_error("Duplicate function: " + name);
        }
    };
    const auto add = [&](const std::string& function, Word left, Word right, Word result,
                         Memory memory = kMemory, Memory expected_memory = kMemory) {
        cases.push_back(Case{function + "." + std::to_string(cases.size()), function,
                             left, right, memory, result, expected_memory});
    };

    // The reference results below use C++ arithmetic on architectural values,
    // independently of the instruction sequences used by Biscuit.
    const Word all = std::numeric_limits<Word>::max();
    const Word sign = Word{1} << 63;
    const std::array<Word, 4> constants{0, all, sign, 0x123456789abcdef0};
    for (std::size_t i = 0; i < constants.size(); ++i) {
        const auto name = "constant_" + std::to_string(i);
        emit(name, [&](Assembler& as) { as.LI(a0, constants[i]); as.RET(); });
        add(name, 19, 23, constants[i]);
    }

    emit("identity", [](Assembler& as) { as.RET(); });
    for (Word value : {Word{42}, all, sign}) add("identity", value, 0, value);

    emit("add", [](Assembler& as) { as.ADD(a0, a0, a1); as.RET(); });
    emit("subtract", [](Assembler& as) { as.SUB(a0, a0, a1); as.RET(); });
    emit("add_u32", [](Assembler& as) {
        as.ADDW(a0, a0, a1);
        // Biscuit's ZEXTW uses Zba. The baseline must work without it.
        as.SLLI(a0, a0, 32);
        as.SRLI(a0, a0, 32);
        as.RET();
    });
    const std::array<std::array<Word, 2>, 5> pairs{{
            {19, 23}, {0, 1}, {all, 1}, {sign - 1, 1},
            {0xdeadbeefffffffff, 0x1234567800000002}}};
    for (const auto& pair : pairs) {
        add("add", pair[0], pair[1], pair[0] + pair[1]);
        add("subtract", pair[0], pair[1], pair[0] - pair[1]);
        add("add_u32", pair[0], pair[1], (pair[0] + pair[1]) & 0xffffffff);
    }

    emit("less_unsigned", [](Assembler& as) {
        Label less;
        as.BLTU(a0, a1, &less);
        as.LI(a0, 0);
        as.RET();
        as.Bind(&less);
        as.LI(a0, 1);
        as.RET();
    });
    emit("less_signed", [](Assembler& as) {
        Label less;
        as.BLT(a0, a1, &less);
        as.LI(a0, 0);
        as.RET();
        as.Bind(&less);
        as.LI(a0, 1);
        as.RET();
    });
    for (const auto& pair : std::array<std::array<Word, 2>, 4>{{
            {0, 1}, {1, 0}, {sign, 0}, {all, sign}}}) {
        add("less_unsigned", pair[0], pair[1], pair[0] < pair[1]);
        add("less_signed", pair[0], pair[1],
            std::bit_cast<std::int64_t>(pair[0]) < std::bit_cast<std::int64_t>(pair[1]));
    }
    emit("sum_loop", [](Assembler& as) {
        Label loop, done;
        as.MV(t0, a0);
        as.LI(a0, 0);
        as.Bind(&loop);
        as.BEQ(t0, zero, &done);
        as.ADD(a0, a0, t0);
        as.ADDI(t0, t0, -1);
        as.J(&loop);
        as.Bind(&done);
        as.RET();
    });
    for (Word value : {Word{0}, Word{1}, Word{4}, Word{17}}) {
        add("sum_loop", value, 0, value * (value + 1) / 2);
    }

    emit("load_u8", [](Assembler& as) { as.LBU(a0, 0, a2); as.RET(); });
    emit("load_i8", [](Assembler& as) { as.LB(a0, 0, a2); as.RET(); });
    emit("load_u32", [](Assembler& as) { as.LWU(a0, 0, a2); as.RET(); });
    emit("load_i32", [](Assembler& as) { as.LW(a0, 0, a2); as.RET(); });
    emit("load_u64_offset", [](Assembler& as) { as.LD(a0, 8, a2); as.RET(); });
    for (Word value : {Word{0x123456787fffff7f}, Word{0xfedcba9880000080}}) {
        auto memory = kMemory;
        memory[0] = value;
        const auto byte = value & 0xff;
        const auto word = value & 0xffffffff;
        add("load_u8", 0, 0, byte, memory, memory);
        add("load_i8", 0, 0, byte & 0x80 ? byte | ~Word{0xff} : byte, memory, memory);
        add("load_u32", 0, 0, word, memory, memory);
        add("load_i32", 0, 0, word & 0x80000000 ? word | ~Word{0xffffffff} : word,
            memory, memory);
    }
    add("load_u64_offset", 0, 0, kMemory[1]);
    emit("store_u64", [](Assembler& as) { as.SD(a0, 0, a2); as.LD(a0, 0, a2); as.RET(); });
    emit("store_u32", [](Assembler& as) { as.SW(a0, 0, a2); as.LWU(a0, 0, a2); as.RET(); });
    for (Word value : {all, Word{0x1234567880000000}}) {
        auto expected = kMemory;
        expected[0] = value;
        add("store_u64", value, 0, value, kMemory, expected);
        expected[0] = (kMemory[0] & 0xffffffff00000000) | (value & 0xffffffff);
        add("store_u32", value, 0, value & 0xffffffff, kMemory, expected);
    }

    emit("nested_call", [](Assembler& as) {
        Label inner;
        as.ADDI(sp, sp, -16);
        as.SD(ra, 8, sp);
        as.JAL(ra, &inner);
        as.LD(ra, 8, sp);
        as.ADDI(sp, sp, 16);
        as.RET();
        as.Bind(&inner);
        as.ADD(a0, a0, a1);
        as.RET();
    });
    add("nested_call", 19, 23, 42);
    add("nested_call", all, 1, 0);

    for (const auto& [name, function] : functions) {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(directory / (name + ".bin"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(function.bytes.data()), function.bytes.size());
        file.close();
    }
    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open(directory / "manifest.json");
    manifest << "{\n\"format_version\":1,\"isa\":\"rv64g\",\"abi\":\"lp64d\",\n\"functions\":[\n";
    bool first = true;
    for (const auto& [name, function] : functions) {
        if (!first) manifest << ",\n";
        first = false;
        manifest << "{\"name\":\"" << name << "\",\"file\":\"" << name
                 << ".bin\",\"size\":" << function.bytes.size()
                 << ",\"cookie\":" << function.cookie << '}';
    }
    manifest << "\n],\"cases\":[\n";
    first = true;
    for (const auto& item : cases) {
        if (!first) manifest << ",\n";
        first = false;
        manifest << "{\"name\":\"" << item.name << "\",\"function\":\"" << item.function
                 << "\",\"args\":[\"" << Hex(item.left) << "\",\"" << Hex(item.right)
                 << "\"],\"memory\":";
        WriteMemory(manifest, item.memory);
        manifest << ",\"expected_result\":\"" << Hex(item.result) << "\",\"expected_memory\":";
        WriteMemory(manifest, item.expected_memory);
        manifest << '}';
    }
    manifest << "\n]}\n";
    manifest.close();
    std::cout << "Emitted " << functions.size() << " RV64G functions and " << cases.size()
              << " scenarios to " << directory << ". Execution is checked by run_riscv_smoke.py.\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: swift_riscv_emit OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        EmitBundle(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RV64 emission failed: " << error.what() << '\n';
        return 1;
    }
}
