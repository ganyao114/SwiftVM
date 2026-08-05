#include "runtime/common/svm_config.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>

#include "runtime/common/perf_stats.h"

namespace swift::runtime {
namespace {

struct ConfigState {
    std::mutex reload_mutex;
    std::unique_ptr<std::once_flag> once{std::make_unique<std::once_flag>()};
    SvmConfig config{};
};

ConfigState& State() {
    static ConfigState state;
    return state;
}

bool ParsePresence(const char* value, bool fallback) {
    return value ? true : fallback;
}

bool ParseNonZero(const char* value, bool fallback) {
    return value ? std::strcmp(value, "0") != 0 : fallback;
}

bool ParseNonZeroNonEmpty(const char* value, bool fallback) {
    return value ? value[0] != '\0' && std::strcmp(value, "0") != 0 : fallback;
}

bool ParseDefaultOn(const char* value, bool fallback) {
    return value ? std::strcmp(value, "0") != 0 : fallback;
}

bool ParseEqualsOne(const char* value, bool fallback) {
    return value ? std::strcmp(value, "1") == 0 : fallback;
}

bool ParseInheritAvx(const char* value, bool fallback) {
    return value ? std::strcmp(value, "0") != 0 : fallback;
}

std::string ParseRawString(const char* value, const char* fallback) {
    return value ? value : fallback;
}

// 仅用于让非模板 X-macro 的 if constexpr 丢弃分支保持良构；字符串行实际走
// 上面的 const char* overload。
std::string ParseRawString(const char* value, bool) {
    return value ? value : "";
}

u64 ParseFuncLazy(const char* value, u64 fallback) {
    if (!value) return fallback;
    const long parsed = std::strtol(value, nullptr, 10);
    return parsed <= 0 ? 1024 : static_cast<u64>(parsed);
}

u32 ParseNonNegativeAtoi(const char* value, u32 fallback) {
    if (!value) return fallback;
    return static_cast<u32>(std::max(0, std::atoi(value)));
}

u32 ParseCappedU32_64(const char* value, u32 fallback) {
    if (!value) return fallback;
    return static_cast<u32>(std::min(std::strtoul(value, nullptr, 10), 64ul));
}

u32 ParseStressIterations(const char* value, u32 fallback) {
    return value ? static_cast<u32>(std::max(0, std::atoi(value))) : fallback;
}

u32 ParseStressThreads(const char* value, u32) {
    if (value) {
        return static_cast<u32>(std::clamp(std::atoi(value), 1, 8));
    }
    return std::clamp(std::thread::hardware_concurrency(), 2u, 4u);
}

u64 ParseEmptyBenchIterations(const char* value, u64 fallback) {
    if (!value || std::strcmp(value, "0") == 0) return fallback;
    const auto parsed = static_cast<u64>(std::strtoull(value, nullptr, 10));
    return parsed < 1000 ? 1000000 : parsed;
}

std::string Canonical(bool value) { return value ? "1" : "0"; }
std::string Canonical(u32 value) { return std::to_string(value); }
std::string Canonical(u64 value) { return std::to_string(value); }
std::string Canonical(const std::string& value) { return value; }

template <typename T>
std::string CanonicalKnown(const T& value, bool) {
    return Canonical(value);
}

std::string CanonicalKnown(const std::string& value, bool is_set) {
    // 字符串字段有些以“是否存在”参与语义（profile 空串、guest_bits 空串等）。
    // 保留 presence 标记，仍以单条 name=value 进入哈希。
    return std::string{is_set ? "set:" : "unset:"} + value;
}

void ParseConfig(SvmConfig& config) {
    // XSAVE_YMM 的缺省值继承已解析的 AVX；表顺序保证 AVX 在它之前。
#define SVM_PARSE_FIELD(type, field, name, parse, default_value, source_comment) \
    do { \
        const char* raw = PerfGetenv(name); \
        config.field##_is_set = raw != nullptr; \
        if constexpr (std::is_same_v<type, bool>) { \
            const bool fallback = std::string_view{name} == "SVM_XSAVE_YMM" \
                    ? config.avx \
                    : static_cast<bool>(default_value); \
            config.field = Parse##parse(raw, fallback); \
        } else { \
            config.field = Parse##parse(raw, default_value); \
        } \
    } while (false);
    SVM_CONFIG_FIELDS(SVM_PARSE_FIELD)
#undef SVM_PARSE_FIELD

    if (!config.direct_link_stress_iters_is_set &&
        config.direct_link_stress_long) {
        config.direct_link_stress_iters = 1'000'000;
    }

}

}  // namespace

bool FeatureSet::Get(FeatureId id) const {
    switch (id) {
#define SVM_FEATURE_GET(field, default_value) case FeatureId::field: return field;
        SVM_FEATURE_FIELDS(SVM_FEATURE_GET)
#undef SVM_FEATURE_GET
        case FeatureId::Count: break;
    }
    return false;
}

void FeatureSet::Set(FeatureId id, bool value) {
    switch (id) {
#define SVM_FEATURE_SET(field, default_value) case FeatureId::field: field = value; return;
        SVM_FEATURE_FIELDS(SVM_FEATURE_SET)
#undef SVM_FEATURE_SET
        case FeatureId::Count: return;
    }
}

std::string_view FeatureName(FeatureId id) {
    switch (id) {
#define SVM_FEATURE_NAME(field, default_value) case FeatureId::field: return #field;
        SVM_FEATURE_FIELDS(SVM_FEATURE_NAME)
#undef SVM_FEATURE_NAME
        case FeatureId::Count: break;
    }
    return {};
}

std::optional<FeatureId> FeatureIdFromName(std::string_view name) {
#define SVM_FEATURE_BY_NAME(field, default_value) \
    if (name == #field) return FeatureId::field;
    SVM_FEATURE_FIELDS(SVM_FEATURE_BY_NAME)
#undef SVM_FEATURE_BY_NAME
    return std::nullopt;
}

u64 HashFeatureSet(const FeatureSet& features) {
    constexpr u64 kFnvOffset = 0xCBF29CE484222325ull;
    constexpr u64 kFnvPrime = 0x100000001B3ull;
    u64 hash = kFnvOffset;
    auto mix = [&](u8 byte) {
        hash ^= byte;
        hash *= kFnvPrime;
    };
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const auto id = static_cast<FeatureId>(i);
        for (u32 shift = 0; shift < 64; shift += 8) {
            mix(static_cast<u8>((static_cast<u64>(i) >> shift) & 0xff));
        }
        mix(features.Get(id) ? 1 : 0);
    }
    return hash;
}

bool FeatureOverrides::Empty() const {
    return std::none_of(values.begin(), values.end(),
                        [](const auto& value) { return value.has_value(); });
}

std::optional<bool> FeatureOverrides::Get(FeatureId id) const {
    if (id == FeatureId::Count) return std::nullopt;
    return values[static_cast<std::size_t>(id)];
}

void FeatureOverrides::Set(FeatureId id, bool value) {
    if (id != FeatureId::Count) values[static_cast<std::size_t>(id)] = value;
}

FeatureSet SvmConfig::GetFeatureSet() const {
    FeatureSet features;
#define SVM_FEATURE_COPY(field, default_value) features.field = field;
    SVM_FEATURE_FIELDS(SVM_FEATURE_COPY)
#undef SVM_FEATURE_COPY
    return features;
}

const SvmConfig& GetSvmConfig() {
    auto& state = State();
    std::call_once(*state.once, [&state] { ParseConfig(state.config); });
    return state.config;
}

void InitSvmConfig() { (void)GetSvmConfig(); }

void ReloadSvmConfigForTest() {
    auto& state = State();
    std::lock_guard lock(state.reload_mutex);
    state.config = {};
    state.once = std::make_unique<std::once_flag>();
}

const char* GetRawSvmConfigEnvForTest(const char* name) {
    return std::getenv(name);
}

int SetSvmConfigEnvForTest(const char* name, const char* value, int overwrite) {
    const int result = ::setenv(name, value, overwrite);
    if (result == 0) ReloadSvmConfigForTest();
    return result;
}

int UnsetSvmConfigEnvForTest(const char* name) {
    const int result = ::unsetenv(name);
    if (result == 0) ReloadSvmConfigForTest();
    return result;
}

void EnableSvmX86AbiBaselineForDriver() {
    auto& state = State();
    std::lock_guard lock(state.reload_mutex);
    (void)GetSvmConfig();
    state.config.x86_64_abi_baseline = true;
    state.config.x86_64_abi_baseline_is_set = true;
}

bool SvmVixlProfEnabled() { return GetSvmConfig().vixl_prof; }
bool IsKnownSvmConfigName(std::string_view name) {
    return std::find(kSvmConfigEnvNames.begin(), kSvmConfigEnvNames.end(), name) !=
           kSvmConfigEnvNames.end();
}

std::vector<std::string> SerializeSvmConfig(const SvmConfig& config) {
    std::vector<std::string> entries;
    entries.reserve(kSvmConfigFieldCount);
#define SVM_SERIALIZE_FIELD(type, field, name, parse, default_value, source_comment) \
    entries.emplace_back(std::string{name} + "=" + \
                         CanonicalKnown(config.field, config.field##_is_set));
    SVM_CONFIG_FIELDS(SVM_SERIALIZE_FIELD)
#undef SVM_SERIALIZE_FIELD
    return entries;
}

const char* PerfGetenv(const char* name) {
    // SvmConfig 初始化期间不能反向调用 Perf2Enabled()（它依赖 SvmConfig）。
    // 这里只为初始化批量读取保留一次 raw bootstrap；其余代码不再调用 PerfGetenv。
    const bool perf2_enabled = std::getenv("SVM_PROF2") != nullptr;
    if (!perf2_enabled || !perf2_translation_active) {
        return std::getenv(name);
    }
    auto& stats = GetPerfStats2();
    const auto begin = std::chrono::steady_clock::now();
    const char* value = std::getenv(name);
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - begin)
                            .count();
    stats.translate_probe_calls.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < stats.kGetenvNames.size(); ++i) {
        if (std::strcmp(name, stats.kGetenvNames[i]) == 0) {
            stats.getenv_calls[i].fetch_add(1, std::memory_order_relaxed);
            stats.getenv_ns[i].fetch_add(static_cast<unsigned long long>(ns),
                                         std::memory_order_relaxed);
            break;
        }
    }
    return value;
}

}  // namespace swift::runtime
