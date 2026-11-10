#include "case.h"

#include "base/error.h"
#include "base/hash.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <type_traits>

namespace swift::tests::correctness {
namespace {

constexpr std::array<std::uint8_t, 8> Magic{'S', 'V', 'M', 'C', 'A', 'S', 'E', 0};
constexpr std::size_t MaxCaseBytes = 1024 * 1024, MaxJSONBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxCodeBytes = 4096, MaxRegions = 16, MaxMemory = 16 * 1024 * 1024;

[[noreturn]] void Invalid(std::string_view message) {
    swift::ThrowError("correctness protocol: " + std::string(message),
                      swift::ErrorKind::InvalidArgument);
}

struct Writer {
    std::vector<std::uint8_t> data;

    template <class T> void Integer(T value) {
        using U = std::make_unsigned_t<T>;
        const auto bits = static_cast<U>(value);
        for (unsigned i = 0; i < sizeof(T); ++i)
            data.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    }

    void Raw(std::span<const std::uint8_t> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }

    void Blob(std::span<const std::uint8_t> bytes) {
        if (bytes.size() > MaxCaseBytes) Invalid("blob too large");
        Integer<std::uint32_t>(bytes.size());
        Raw(bytes);
    }

    void String(std::string_view value) {
        if (value.size() > 256) Invalid("string too long");
        Blob({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
    }
};

struct Reader {
    std::span<const std::uint8_t> data;

    template <class T> T Integer() {
        if (data.size() < sizeof(T)) Invalid("truncated integer");
        using U = std::make_unsigned_t<T>;
        U value{};
        for (unsigned i = 0; i < sizeof(T); ++i) value |= U(data[i]) << (i * 8);
        data = data.subspan(sizeof(T));
        return static_cast<T>(value);
    }

    void Raw(std::span<std::uint8_t> destination) {
        if (data.size() < destination.size()) Invalid("truncated byte field");
        std::copy_n(data.begin(), destination.size(), destination.begin());
        data = data.subspan(destination.size());
    }

    std::vector<std::uint8_t> Blob(std::size_t limit) {
        const auto size = Integer<std::uint32_t>();
        if (size > limit || data.size() < size) Invalid("invalid blob size");
        std::vector<std::uint8_t> result(data.begin(), data.begin() + size);
        data = data.subspan(size);
        return result;
    }

    std::string String() {
        const auto bytes = Blob(256);
        return {bytes.begin(), bytes.end()};
    }
};

void EncodeState(Writer& out, const State& state) {
    for (const auto value : state.gpr) out.Integer(value);
    out.Integer(state.pc);
    out.Integer(state.flags);
    for (const auto& vector : state.vectors) out.Raw(vector);
    out.Integer(state.fp_control);
    out.Integer(state.fp_status);
    for (const auto value : state.tls) out.Integer(value);
    for (const auto& value : state.x87) out.Raw(value);
    out.Integer(state.x87_control);
    out.Integer(state.x87_status);
    out.Integer(state.x87_tag);
    out.Integer(state.x87_opcode);
    out.Integer(state.x87_ip);
    out.Integer(state.x87_dp);
}

State DecodeState(Reader& in) {
    State state;
    for (auto& value : state.gpr) value = in.Integer<std::uint64_t>();
    state.pc = in.Integer<std::uint64_t>();
    state.flags = in.Integer<std::uint64_t>();
    for (auto& vector : state.vectors) in.Raw(vector);
    state.fp_control = in.Integer<std::uint32_t>();
    state.fp_status = in.Integer<std::uint32_t>();
    for (auto& value : state.tls) value = in.Integer<std::uint64_t>();
    for (auto& value : state.x87) in.Raw(value);
    state.x87_control = in.Integer<std::uint16_t>();
    state.x87_status = in.Integer<std::uint16_t>();
    state.x87_tag = in.Integer<std::uint16_t>();
    state.x87_opcode = in.Integer<std::uint16_t>();
    state.x87_ip = in.Integer<std::uint64_t>();
    state.x87_dp = in.Integer<std::uint64_t>();
    return state;
}

void CheckStructure(const Case& c) {
    if (c.isa != ISA::X86_64 && c.isa != ISA::Arm64 && c.isa != ISA::Arm32) Invalid("unknown ISA");
    if (c.code.empty() || c.code.size() > MaxCodeBytes || c.memory.size() > MaxRegions ||
        c.code_address > UINT64_MAX - c.code.size() || !c.max_steps || c.max_steps > 1000000)
        Invalid("case bounds");
    std::uint64_t total{};
    for (std::size_t i = 0; i < c.memory.size(); ++i) {
        const auto& region = c.memory[i];
        if (!region.size || region.size > MaxMemory || region.bytes.size() > region.size ||
            region.address > UINT64_MAX - region.size || (region.protection & ~7u))
            Invalid("memory region bounds");
        total += region.size;
        if (total > MaxMemory) Invalid("memory budget exceeded");
        const auto end = region.address + region.size;
        if (region.address < c.code_address + c.code.size() && c.code_address < end)
            Invalid("code/data overlap requires an explicit future scenario contract");
        for (std::size_t j = 0; j < i; ++j)
            if (region.address < c.memory[j].address + c.memory[j].size &&
                c.memory[j].address < end)
                Invalid("overlapping memory regions");
    }
}

std::vector<std::uint8_t> ReadBytes(const std::string& path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) Invalid("cannot open " + path);
    const auto size = file.tellg();
    if (size < 0 || std::uint64_t(size) > limit) Invalid("file size exceeds limit");
    file.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
        Invalid("cannot read " + path);
    return bytes;
}

void Quote(std::string& out, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

template <class T> void Number(std::string& out, T value) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
    out.append(buffer, result.ptr);
}

template <class T> void NumberArray(std::string& out, const T& values) {
    out += '[';
    bool first = true;
    for (const auto value : values) {
        if (!first) out += ',';
        first = false;
        Number(out, value);
    }
    out += ']';
}

template <class T> void Field(std::string& out, std::string_view key, T value) {
    out += ',';
    Quote(out, key);
    out += ':';
    Number(out, value);
}

void StringField(std::string& out, std::string_view key, std::string_view value) {
    out += ',';
    Quote(out, key);
    out += ':';
    Quote(out, value);
}

void StateJSON(std::string& out, const State& state) {
    out += "{\"gpr\":";
    NumberArray(out, state.gpr);
    Field(out, "pc", state.pc);
    Field(out, "flags", state.flags);
    out += ",\"vectors\":[";
    for (unsigned i = 0; i < state.vectors.size(); ++i) {
        if (i) out += ',';
        NumberArray(out, state.vectors[i]);
    }
    out += ']';
    Field(out, "fp_control", state.fp_control);
    Field(out, "fp_status", state.fp_status);
    out += ",\"tls\":";
    NumberArray(out, state.tls);
    out += ",\"x87\":[";
    for (unsigned i = 0; i < state.x87.size(); ++i) {
        if (i) out += ',';
        NumberArray(out, state.x87[i]);
    }
    out += ']';
    Field(out, "x87_control", state.x87_control);
    Field(out, "x87_status", state.x87_status);
    Field(out, "x87_tag", state.x87_tag);
    Field(out, "x87_opcode", state.x87_opcode);
    Field(out, "x87_ip", state.x87_ip);
    Field(out, "x87_dp", state.x87_dp);
    out += '}';
}

void MemoryJSON(std::string& out, const std::vector<MemoryRegion>& regions) {
    out += '[';
    bool first = true;
    for (const auto& region : regions) {
        if (!first) out += ',';
        first = false;
        out += "{\"address\":";
        Number(out, region.address);
        Field(out, "size", region.size);
        Field(out, "protection", region.protection);
        out += ",\"bytes\":";
        NumberArray(out, region.bytes);
        out += '}';
    }
    out += ']';
}

// This parser is confined to the test wire protocol. Integers retain their
// decimal spelling, so values above 2^53 never pass through floating point.
struct JSON {
    enum Kind { Null, Number, String, Boolean, Array, Object } kind{Null};

    std::string text;
    std::vector<JSON> array;
    std::map<std::string, JSON, std::less<>> object;

    const JSON* Find(std::string_view key) const {
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }

    const JSON& At(std::string_view key) const {
        const auto* value = Find(key);
        if (kind != Object || !value) Invalid("missing JSON field " + std::string(key));
        return *value;
    }
};

class JSONParser {
public:
    explicit JSONParser(std::string_view value) : input(value) {}

    JSON Parse() {
        auto value = Value(0);
        Space();
        if (!input.empty()) Invalid("trailing JSON");
        return value;
    }

private:
    std::string_view input;
    unsigned nodes{};

    void Space() {
        while (!input.empty() && (input.front() == ' ' || input.front() == '\n' ||
                                  input.front() == '\r' || input.front() == '\t'))
            input.remove_prefix(1);
    }

    bool Take(char c) {
        Space();
        if (input.empty() || input.front() != c) return false;
        input.remove_prefix(1);
        return true;
    }

    unsigned HexDigit(char c) {
        if (c >= '0' && c <= '9') return unsigned(c - '0');
        if (c >= 'a' && c <= 'f') return unsigned(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return unsigned(c - 'A' + 10);
        Invalid("invalid JSON unicode escape");
    }

    unsigned Unicode() {
        if (input.size() < 4) Invalid("truncated JSON unicode");
        unsigned value{};
        for (unsigned i = 0; i < 4; ++i) value = value * 16 + HexDigit(input[i]);
        input.remove_prefix(4);
        return value;
    }

    std::string String() {
        if (!Take('"')) Invalid("JSON string expected");
        std::string value;
        while (!input.empty()) {
            const auto c = static_cast<unsigned char>(input.front());
            input.remove_prefix(1);
            if (c == '"') return value;
            if (c < 32) Invalid("control character in JSON string");
            if (c != '\\') {
                value += static_cast<char>(c);
                continue;
            }
            if (input.empty()) Invalid("truncated JSON escape");
            const auto escaped = input.front();
            input.remove_prefix(1);
            if (escaped == '"' || escaped == '\\' || escaped == '/')
                value += escaped;
            else if (escaped == 'b')
                value += '\b';
            else if (escaped == 'f')
                value += '\f';
            else if (escaped == 'n')
                value += '\n';
            else if (escaped == 'r')
                value += '\r';
            else if (escaped == 't')
                value += '\t';
            else if (escaped == 'u') {
                auto cp = Unicode();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    if (input.size() < 2 || input.substr(0, 2) != "\\u") Invalid("JSON surrogate");
                    input.remove_prefix(2);
                    const auto low = Unicode();
                    if (low < 0xdc00 || low > 0xdfff) Invalid("JSON surrogate");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff)
                    Invalid("JSON surrogate");
                if (cp < 0x80)
                    value += static_cast<char>(cp);
                else if (cp < 0x800) {
                    value += static_cast<char>(0xc0 | (cp >> 6));
                    value += static_cast<char>(0x80 | (cp & 63));
                } else if (cp < 0x10000) {
                    value += static_cast<char>(0xe0 | (cp >> 12));
                    value += static_cast<char>(0x80 | ((cp >> 6) & 63));
                    value += static_cast<char>(0x80 | (cp & 63));
                } else {
                    value += static_cast<char>(0xf0 | (cp >> 18));
                    value += static_cast<char>(0x80 | ((cp >> 12) & 63));
                    value += static_cast<char>(0x80 | ((cp >> 6) & 63));
                    value += static_cast<char>(0x80 | (cp & 63));
                }
            } else
                Invalid("invalid JSON escape");
        }
        Invalid("unterminated JSON string");
    }

    JSON Value(unsigned depth) {
        if (depth > 16 || ++nodes > 2000000) Invalid("JSON nesting/node budget");
        Space();
        if (input.empty()) Invalid("truncated JSON");
        JSON out;
        if (input.front() == '"') {
            out.kind = JSON::String;
            out.text = String();
        } else if (Take('{')) {
            out.kind = JSON::Object;
            if (Take('}')) return out;
            do {
                const auto key = String();
                if (!Take(':')) Invalid("JSON colon expected");
                if (!out.object.emplace(key, Value(depth + 1)).second)
                    Invalid("duplicate JSON key");
            } while (Take(','));
            if (!Take('}')) Invalid("JSON object not closed");
        } else if (Take('[')) {
            out.kind = JSON::Array;
            if (Take(']')) return out;
            do {
                out.array.push_back(Value(depth + 1));
            } while (Take(','));
            if (!Take(']')) Invalid("JSON array not closed");
        } else if (input.starts_with("true") || input.starts_with("false")) {
            out.kind = JSON::Boolean;
            const auto length = input.front() == 't' ? 4 : 5;
            out.text = input.substr(0, length);
            input.remove_prefix(length);
        } else if (input.starts_with("null")) {
            input.remove_prefix(4);
        } else {
            out.kind = JSON::Number;
            std::size_t length = input.front() == '-' ? 1 : 0;
            const auto first = length;
            while (length < input.size() && input[length] >= '0' && input[length] <= '9') ++length;
            if (length == first || (length - first > 1 && input[first] == '0'))
                Invalid("JSON integer expected");
            // Diagnostic metadata may contain fractional timings. Preserve its
            // spelling as well; Numeric<T> still rejects non-integer values in
            // architectural fields instead of rounding them through double.
            if (length < input.size() && input[length] == '.') {
                const auto fraction = ++length;
                while (length < input.size() && input[length] >= '0' && input[length] <= '9')
                    ++length;
                if (length == fraction) Invalid("JSON fraction expected");
            }
            if (length < input.size() && (input[length] == 'e' || input[length] == 'E')) {
                ++length;
                if (length < input.size() && (input[length] == '+' || input[length] == '-'))
                    ++length;
                const auto exponent = length;
                while (length < input.size() && input[length] >= '0' && input[length] <= '9')
                    ++length;
                if (length == exponent) Invalid("JSON exponent expected");
            }
            out.text = input.substr(0, length);
            input.remove_prefix(length);
        }
        return out;
    }
};

template <class T> T Numeric(const JSON& json) {
    if (json.kind != JSON::Number) Invalid("JSON integer expected");
    T value{};
    const auto parsed =
            std::from_chars(json.text.data(), json.text.data() + json.text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != json.text.data() + json.text.size())
        Invalid("JSON integer out of range");
    return value;
}

std::string Text(const JSON& json) {
    if (json.kind != JSON::String) Invalid("JSON string expected");
    return json.text;
}

bool Boolean(const JSON& json) {
    if (json.kind != JSON::Boolean) Invalid("JSON boolean expected");
    return json.text == "true";
}

template <class T> void ReadArray(const JSON& json, T& values) {
    if (json.kind != JSON::Array || json.array.size() != values.size())
        Invalid("JSON array dimensions");
    for (unsigned i = 0; i < values.size(); ++i)
        values[i] = Numeric<std::remove_reference_t<decltype(values[i])>>(json.array[i]);
}

template <class T> void Optional(const JSON& json, std::string_view key, T& value) {
    if (const auto* member = json.Find(key)) {
        if constexpr (std::is_same_v<T, std::string>)
            value = Text(*member);
        else if constexpr (std::is_same_v<T, bool>)
            value = Boolean(*member);
        else
            value = Numeric<T>(*member);
    }
}

State ReadState(const JSON& json) {
    State state;
    ReadArray(json.At("gpr"), state.gpr);
    state.pc = Numeric<std::uint64_t>(json.At("pc"));
    state.flags = Numeric<std::uint64_t>(json.At("flags"));
    const auto& vectors = json.At("vectors");
    if (vectors.kind != JSON::Array || vectors.array.size() != state.vectors.size())
        Invalid("JSON vector bank");
    for (unsigned i = 0; i < state.vectors.size(); ++i)
        ReadArray(vectors.array[i], state.vectors[i]);
    state.fp_control = Numeric<std::uint32_t>(json.At("fp_control"));
    state.fp_status = Numeric<std::uint32_t>(json.At("fp_status"));
    ReadArray(json.At("tls"), state.tls);
    const auto& x87 = json.At("x87");
    if (x87.kind != JSON::Array || x87.array.size() != state.x87.size()) Invalid("JSON x87 bank");
    for (unsigned i = 0; i < state.x87.size(); ++i) ReadArray(x87.array[i], state.x87[i]);
    Optional(json, "x87_control", state.x87_control);
    Optional(json, "x87_status", state.x87_status);
    Optional(json, "x87_tag", state.x87_tag);
    Optional(json, "x87_opcode", state.x87_opcode);
    Optional(json, "x87_ip", state.x87_ip);
    Optional(json, "x87_dp", state.x87_dp);
    return state;
}

JSON ReadJSON(const std::string& path) {
    const auto bytes = ReadBytes(path, MaxJSONBytes);
    auto json = JSONParser({reinterpret_cast<const char*>(bytes.data()), bytes.size()}).Parse();
    if (const auto* version = json.Find("protocol_version"))
        if (Numeric<std::uint32_t>(*version) != 1) Invalid("unsupported JSON protocol version");
    return json;
}

Scope ReadScope(const JSON& json) {
    const auto value = Text(json);
    if (value == "block") return Scope::Block;
    if (value == "function") return Scope::Function;
    Invalid("unknown allocation scope");
}

}  // namespace

std::string_view Name(ISA value) {
    switch (value) {
        case ISA::X86_64:
            return "x86_64";
        case ISA::Arm64:
            return "arm64";
        case ISA::Arm32:
            return "arm32";
    }
    return "unknown";
}

std::string_view Name(Backend value) {
    switch (value) {
        case Backend::Direct:
            return "direct";
        case Backend::Arm64:
            return "arm64";
        case Backend::X64:
            return "x64";
        case Backend::Riscv64:
            return "riscv64";
        case Backend::LoongArch64:
            return "loongarch64";
    }
    return "unknown";
}

std::string_view Name(Outcome value) {
    switch (value) {
        case Outcome::Terminal:
            return "terminal";
        case Outcome::Fault:
            return "fault";
        case Outcome::Service:
            return "service";
        case Outcome::Rejected:
            return "rejected";
        case Outcome::Budget:
            return "budget";
        case Outcome::Unavailable:
            return "unavailable";
        case Outcome::HostFailure:
            return "host_failure";
        case Outcome::Compiled:
            return "compiled";
    }
    return "unknown";
}

ISA ParseISA(std::string_view value) {
    if (value == "x86_64" || value == "x86-64" || value == "x64") return ISA::X86_64;
    if (value == "arm64" || value == "a64" || value == "aarch64") return ISA::Arm64;
    if (value == "arm32" || value == "a32") return ISA::Arm32;
    Invalid("unknown ISA");
}

Backend ParseBackend(std::string_view value) {
    if (value == "direct") return Backend::Direct;
    if (value == "arm64" || value == "a64") return Backend::Arm64;
    if (value == "x64" || value == "x86_64") return Backend::X64;
    if (value == "riscv64" || value == "rv64") return Backend::Riscv64;
    if (value == "loongarch64" || value == "la64") return Backend::LoongArch64;
    Invalid("unknown backend");
}

std::vector<std::uint8_t> Encode(const Case& c) {
    CheckStructure(c);
    Writer out;
    out.Raw(Magic);
    out.Integer<std::uint32_t>(Case::SchemaVersion);
    out.Integer<std::uint8_t>(static_cast<std::uint8_t>(c.isa));
    out.String(c.profile);
    out.String(c.form_id);
    out.Integer(c.seed);
    out.Integer(c.id);
    out.Integer(c.code_address);
    out.Integer(c.terminal_pc);
    out.Integer(c.max_steps);
    out.Blob(c.code);
    EncodeState(out, c.initial);
    out.Integer<std::uint32_t>(c.memory.size());
    for (const auto& region : c.memory) {
        out.Integer(region.address);
        out.Integer(region.size);
        out.Integer(region.protection);
        out.Blob(region.bytes);
    }
    if (out.data.size() > MaxCaseBytes) Invalid("encoded case too large");
    return std::move(out.data);
}

Case Decode(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > MaxCaseBytes) Invalid("encoded case too large");
    Reader in{bytes};
    std::array<std::uint8_t, 8> magic;
    in.Raw(magic);
    if (magic != Magic || in.Integer<std::uint32_t>() != Case::SchemaVersion)
        Invalid("unsupported case schema");
    Case c;
    c.isa = static_cast<ISA>(in.Integer<std::uint8_t>());
    c.profile = in.String();
    c.form_id = in.String();
    c.seed = in.Integer<std::uint64_t>();
    c.id = in.Integer<std::uint64_t>();
    c.code_address = in.Integer<std::uint64_t>();
    c.terminal_pc = in.Integer<std::uint64_t>();
    c.max_steps = in.Integer<std::uint32_t>();
    c.code = in.Blob(MaxCodeBytes);
    c.initial = DecodeState(in);
    const auto count = in.Integer<std::uint32_t>();
    if (count > MaxRegions) Invalid("too many memory regions");
    for (unsigned i = 0; i < count; ++i) {
        MemoryRegion region;
        region.address = in.Integer<std::uint64_t>();
        region.size = in.Integer<std::uint32_t>();
        region.protection = in.Integer<std::uint8_t>();
        region.bytes = in.Blob(MaxCaseBytes);
        c.memory.push_back(std::move(region));
    }
    if (!in.data.empty()) Invalid("trailing case bytes");
    CheckStructure(c);
    return c;
}

Case ReadCase(const std::string& path) { return Decode(ReadBytes(path, MaxCaseBytes)); }

void WriteCase(const std::string& path, const Case& c) {
    const auto bytes = Encode(c);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        Invalid("cannot write " + path);
}

std::string CaseHash(const Case& c) { return swift::HexDigest(swift::HashBytes(Encode(c))); }

std::string ToJSON(const Case& c) {
    std::string out = "{\"protocol_version\":1,\"schema_version\":1";
    StringField(out, "isa", Name(c.isa));
    StringField(out, "profile", c.profile);
    StringField(out, "form_id", c.form_id);
    StringField(out, "case_hash", CaseHash(c));
    Field(out, "seed", c.seed);
    Field(out, "id", c.id);
    Field(out, "code_address", c.code_address);
    Field(out, "terminal_pc", c.terminal_pc);
    Field(out, "max_steps", c.max_steps);
    out += ",\"code\":";
    NumberArray(out, c.code);
    out += ",\"initial\":";
    StateJSON(out, c.initial);
    out += ",\"memory\":";
    MemoryJSON(out, c.memory);
    out += '}';
    return out;
}

std::string ToJSON(const Observation& o) {
    std::string out = "{\"protocol_version\":1";
    StringField(out, "outcome", Name(o.outcome));
    StringField(out, "stage", o.stage);
    StringField(out, "error", o.error);
    StringField(out, "backend", o.backend);
    out += ",\"state\":";
    StateJSON(out, o.state);
    out += ",\"memory\":";
    MemoryJSON(out, o.memory);
    StringField(out, "fault_kind", o.fault_kind);
    StringField(out, "fault_access", o.fault_access);
    Field(out, "fault_pc", o.fault_pc);
    Field(out, "fault_address", o.fault_address);
    Field(out, "fault_width", o.fault_width);
    Field(out, "fault_vector", o.fault_vector);
    Field(out, "service", o.service);
    Field(out, "steps", o.steps);
    out += o.fault_address_valid ? ",\"fault_address_valid\":true"
                                 : ",\"fault_address_valid\":false";
    out += o.native_executed ? ",\"native_executed\":true" : ",\"native_executed\":false";
    Field(out, "compile_attempts", o.compile_attempts);
    Field(out, "compiled_units", o.compiled_units);
    Field(out, "interpreted_steps", o.interpreted_steps);
    Field(out, "fallbacks", o.fallbacks);
    Field(out, "actual_ir_blocks", o.actual_ir_blocks);
    Field(out, "actual_source_blocks", o.actual_source_blocks);
    Field(out, "spill_slots", o.spill_slots);
    Field(out, "frame_bytes", o.frame_bytes);
    Field(out, "fp_control_verified_mask", o.fp_control_verified_mask);
    StringField(out, "actual_scope", o.actual_scope == Scope::Block ? "block" : "function");
    out += '}';
    return out;
}

std::string ToJSON(const RunConfig& c) {
    std::string out = "{\"protocol_version\":1";
    StringField(out, "backend", Name(c.backend));
    StringField(out, "scope", c.scope == Scope::Block ? "block" : "function");
    StringField(out,
                "target",
                c.target == TargetMode::Known      ? "known"
                : c.target == TargetMode::EarlyOff ? "early_off"
                                                   : "unknown");
    out += c.optimize ? ",\"optimize\":true" : ",\"optimize\":false";
    out += c.pin ? ",\"pin\":true" : ",\"pin\":false";
    out += c.compile_only ? ",\"compile_only\":true}" : ",\"compile_only\":false}";
    return out;
}

std::string ToJSON(const Comparison& c) {
    std::string out = c.comparable ? "{\"comparable\":true" : "{\"comparable\":false";
    out += c.equal ? ",\"equal\":true" : ",\"equal\":false";
    StringField(out, "category", c.category);
    StringField(out, "detail", c.detail);
    out += '}';
    return out;
}

Observation ReadObservation(const std::string& path) {
    const auto json = ReadJSON(path);
    Observation o;
    const auto outcome = Text(json.At("outcome"));
    bool found{};
    for (unsigned i = 0; i <= unsigned(Outcome::Compiled); ++i)
        if (Name(static_cast<Outcome>(i)) == outcome) {
            o.outcome = static_cast<Outcome>(i);
            found = true;
        }
    if (!found) Invalid("unknown observation outcome");
    o.state = ReadState(json.At("state"));
    const auto& regions = json.At("memory");
    if (regions.kind != JSON::Array || regions.array.size() > MaxRegions)
        Invalid("JSON memory regions");
    std::uint64_t total{};
    for (const auto& value : regions.array) {
        MemoryRegion region;
        region.address = Numeric<std::uint64_t>(value.At("address"));
        region.size = Numeric<std::uint32_t>(value.At("size"));
        region.protection = Numeric<std::uint8_t>(value.At("protection"));
        const auto& bytes = value.At("bytes");
        total += region.size;
        if (bytes.kind != JSON::Array || bytes.array.size() > region.size ||
            region.address > UINT64_MAX - region.size || total > MaxMemory ||
            (region.protection & ~7u))
            Invalid("JSON memory bounds");
        region.bytes.reserve(bytes.array.size());
        for (const auto& byte : bytes.array) region.bytes.push_back(Numeric<std::uint8_t>(byte));
        o.memory.push_back(std::move(region));
    }
    Optional(json, "stage", o.stage);
    Optional(json, "error", o.error);
    Optional(json, "backend", o.backend);
    Optional(json, "fault_kind", o.fault_kind);
    Optional(json, "fault_access", o.fault_access);
    Optional(json, "fault_pc", o.fault_pc);
    Optional(json, "fault_address", o.fault_address);
    Optional(json, "fault_width", o.fault_width);
    Optional(json, "fault_vector", o.fault_vector);
    Optional(json, "service", o.service);
    Optional(json, "steps", o.steps);
    Optional(json, "fault_address_valid", o.fault_address_valid);
    Optional(json, "native_executed", o.native_executed);
    Optional(json, "compile_attempts", o.compile_attempts);
    Optional(json, "compiled_units", o.compiled_units);
    Optional(json, "interpreted_steps", o.interpreted_steps);
    Optional(json, "fallbacks", o.fallbacks);
    Optional(json, "actual_ir_blocks", o.actual_ir_blocks);
    Optional(json, "actual_source_blocks", o.actual_source_blocks);
    Optional(json, "spill_slots", o.spill_slots);
    Optional(json, "frame_bytes", o.frame_bytes);
    Optional(json, "fp_control_verified_mask", o.fp_control_verified_mask);
    if (const auto* scope = json.Find("actual_scope")) o.actual_scope = ReadScope(*scope);
    return o;
}

RunConfig ReadConfig(const std::string& path) {
    const auto json = ReadJSON(path);
    RunConfig c;
    c.backend = ParseBackend(Text(json.At("backend")));
    if (const auto* scope = json.Find("scope")) c.scope = ReadScope(*scope);
    if (const auto* target = json.Find("target")) {
        const auto value = Text(*target);
        if (value == "known")
            c.target = TargetMode::Known;
        else if (value == "early_off" || value == "earlyoff")
            c.target = TargetMode::EarlyOff;
        else if (value == "unknown")
            c.target = TargetMode::Unknown;
        else
            Invalid("unknown target mode");
    }
    Optional(json, "optimize", c.optimize);
    Optional(json, "pin", c.pin);
    Optional(json, "compile_only", c.compile_only);
    return c;
}

}  // namespace swift::tests::correctness
