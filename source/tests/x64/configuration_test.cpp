#include <array>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/code_serial.h"
#include "runtime/backend/module.h"
#include "runtime/common/svm_config.h"

namespace {
using namespace swift::runtime;
using namespace swift::runtime::backend;

constexpr std::array kX64Features{
        FeatureId::x64_scalar_cache,
        FeatureId::x64_cfg_allocation,
        FeatureId::x64_flags_residency,
        FeatureId::x64_flags_profitability,
        FeatureId::x64_native_cf_chains,
        FeatureId::x64_vector_cfg,
};
constexpr std::array kX64EnvNames{
        "SVM_X64_SCALAR_CACHE",
        "SVM_X64_CFG_ALLOCATION",
        "SVM_X64_FLAGS_RESIDENCY",
        "SVM_X64_FLAGS_PROFITABILITY",
        "SVM_X64_NATIVE_CF_CHAINS",
        "SVM_X64_VECTOR_CFG",
};

// The supported test setters invalidate the process snapshot. Restore even
// when a REQUIRE throws; this executable has no concurrent config readers.
class SavedX64Environment {
public:
    SavedX64Environment() {
        for (std::size_t i = 0; i < kX64EnvNames.size(); ++i) {
            if (const auto* value = GetRawSvmConfigEnvForTest(kX64EnvNames[i])) {
                saved[i] = value;
            }
        }
    }
    ~SavedX64Environment() {
        for (std::size_t i = 0; i < kX64EnvNames.size(); ++i) {
            if (saved[i]) {
                SetSvmConfigEnvForTest(kX64EnvNames[i], saved[i]->c_str(), 1);
            } else {
                UnsetSvmConfigEnvForTest(kX64EnvNames[i]);
            }
        }
    }
    void Clear() const {
        for (const auto* name : kX64EnvNames) {
            REQUIRE(UnsetSvmConfigEnvForTest(name) == 0);
        }
    }

private:
    std::array<std::optional<std::string>, kX64EnvNames.size()> saved{};
};

Config X64Config() {
    Config config{};
    config.loc_end = 0x10000;
    config.backend_isa = kX86_64;
    config.uniform_buffer_size = 1024;
    return config;
}
}  // namespace

TEST_CASE("x64 configuration registers opt-in defaults and all feature identities",
          "[x64][configuration]") {
    STATIC_REQUIRE(kFeatureCount == 76);
    const FeatureSet defaults{};
    const SvmConfig default_config{};
    for (std::size_t i = 0; i < kX64Features.size(); ++i) {
        CAPTURE(i);
        REQUIRE_FALSE(defaults.Get(kX64Features[i]));
        REQUIRE_FALSE(default_config.GetFeatureSet().Get(kX64Features[i]));
        REQUIRE(IsKnownSvmConfigName(kX64EnvNames[i]));
    }
    REQUIRE(FeatureName(FeatureId::x64_cfg_allocation) == "x64_cfg_allocation");
    REQUIRE(FeatureName(FeatureId::x64_flags_residency) == "x64_flags_residency");
    REQUIRE(FeatureIdFromName("x64_cfg_allocation") == FeatureId::x64_cfg_allocation);
    REQUIRE(FeatureIdFromName("x64_flags_residency") == FeatureId::x64_flags_residency);
    REQUIRE(FeatureName(FeatureId::x64_flags_profitability) == "x64_flags_profitability");
    REQUIRE(FeatureIdFromName("x64_flags_profitability") == FeatureId::x64_flags_profitability);
    REQUIRE(FeatureName(FeatureId::x64_native_cf_chains) == "x64_native_cf_chains");
    REQUIRE(FeatureIdFromName("x64_native_cf_chains") == FeatureId::x64_native_cf_chains);
    REQUIRE(FeatureName(FeatureId::x64_vector_cfg) == "x64_vector_cfg");
    REQUIRE(FeatureIdFromName("x64_vector_cfg") == FeatureId::x64_vector_cfg);

    const auto config = GetSvmConfig();
    const auto captured = config.GetFeatureSet();
#define CHECK_FEATURE_CAPTURE(field, default_value) REQUIRE(captured.field == config.field);
    SVM_FEATURE_FIELDS(CHECK_FEATURE_CAPTURE)
#undef CHECK_FEATURE_CAPTURE
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const auto id = static_cast<FeatureId>(i);
        REQUIRE(FeatureIdFromName(FeatureName(id)) == id);
    }
}

TEST_CASE("x64 environment overrides reload feature and cache environment identities",
          "[x64][configuration][environment]") {
    SavedX64Environment environment;
    for (std::size_t i = 0; i < kX64Features.size(); ++i) {
        CAPTURE(i);
        environment.Clear();
        const auto missing_features = GetSvmConfig().GetFeatureSet();
        const auto missing_env_hash = ComputeEnvHash();
        REQUIRE_FALSE(missing_features.Get(kX64Features[i]));

        REQUIRE(SetSvmConfigEnvForTest(kX64EnvNames[i], "0", 1) == 0);
        REQUIRE_FALSE(GetSvmConfig().GetFeatureSet().Get(kX64Features[i]));
        REQUIRE(ComputeEnvHash() == missing_env_hash);

        REQUIRE(SetSvmConfigEnvForTest(kX64EnvNames[i], "1", 1) == 0);
        const auto enabled_features = GetSvmConfig().GetFeatureSet();
        REQUIRE(enabled_features.Get(kX64Features[i]));
        REQUIRE(ComputeEnvHash() != missing_env_hash);
        REQUIRE(HashFeatureSet(enabled_features) != HashFeatureSet(missing_features));
        for (std::size_t j = 0; j < kFeatureCount; ++j) {
            const auto id = static_cast<FeatureId>(j);
            if (id != kX64Features[i]) {
                REQUIRE(enabled_features.Get(id) == missing_features.Get(id));
            }
        }

        REQUIRE(SetSvmConfigEnvForTest(kX64EnvNames[i], "0", 1) == 0);
        REQUIRE(HashFeatureSet(GetSvmConfig().GetFeatureSet()) ==
                HashFeatureSet(missing_features));
        REQUIRE(ComputeEnvHash() == missing_env_hash);
    }
}

TEST_CASE("x64 sparse module overrides distinguish each independent code policy",
          "[x64][configuration][module]") {
    SavedX64Environment environment;
    environment.Clear();
    const auto config = X64Config();
    const ModuleConfig empty{};
    const auto baseline = ResolveFeatureSet(empty);
    REQUIRE(ComputeConfigHash(config, empty) == ComputeConfigHash(config));
    REQUIRE_FALSE(empty.feature_overrides.Get(FeatureId::x64_cfg_allocation).has_value());
    REQUIRE_FALSE(empty.feature_overrides.Get(FeatureId::x64_flags_residency).has_value());

    std::array<swift::u64, std::size_t{1} << kX64Features.size()> feature_hashes{};
    std::array<swift::u64, std::size_t{1} << kX64Features.size()> config_hashes{};
    for (std::size_t mask = 0; mask < feature_hashes.size(); ++mask) {
        ModuleConfig selected{};
        for (std::size_t i = 0; i < kX64Features.size(); ++i) {
            if ((mask & (std::size_t{1} << i)) != 0) {
                selected.feature_overrides.Set(kX64Features[i], true);
            }
        }
        const auto resolved = ResolveFeatureSet(selected);
        for (std::size_t i = 0; i < kX64Features.size(); ++i) {
            REQUIRE(resolved.Get(kX64Features[i]) == ((mask & (std::size_t{1} << i)) != 0));
        }
        REQUIRE(resolved.const_cse == baseline.const_cse);
        REQUIRE(resolved.uniform_dse == baseline.uniform_dse);
        feature_hashes[mask] = HashFeatureSet(resolved);
        config_hashes[mask] = ComputeConfigHash(config, selected);
        for (std::size_t previous = 0; previous < mask; ++previous) {
            REQUIRE(feature_hashes[mask] != feature_hashes[previous]);
            REQUIRE(config_hashes[mask] != config_hashes[previous]);
        }
    }

    // An explicit module decision takes precedence over the process default.
    for (std::size_t i = 1; i < kX64Features.size(); ++i) {
        REQUIRE(SetSvmConfigEnvForTest(kX64EnvNames[i], "1", 1) == 0);
        ModuleConfig disabled{};
        disabled.feature_overrides.Set(kX64Features[i], false);
        REQUIRE(ResolveFeatureSet(empty).Get(kX64Features[i]));
        REQUIRE_FALSE(ResolveFeatureSet(disabled).Get(kX64Features[i]));
        REQUIRE(ComputeConfigHash(config, disabled) != ComputeConfigHash(config, empty));
        REQUIRE(HashFeatureSet(ResolveFeatureSet(disabled)) !=
                HashFeatureSet(ResolveFeatureSet(empty)));
    }
}

TEST_CASE("x64 mapped feature sets and serialized cache units retain separate identities",
          "[x64][configuration][cache]") {
    SavedX64Environment environment;
    environment.Clear();
    const auto config = X64Config();
    const ModuleConfig default_module{};
    for (const auto feature : {FeatureId::x64_cfg_allocation, FeatureId::x64_flags_residency,
                              FeatureId::x64_flags_profitability, FeatureId::x64_native_cf_chains, FeatureId::x64_vector_cfg}) {
        CAPTURE(FeatureName(feature));
        ModuleConfig off{}, on{};
        off.feature_overrides.Set(FeatureId::const_cse, false);
        on.feature_overrides.Set(FeatureId::const_cse, false);
        off.feature_overrides.Set(feature, false);
        on.feature_overrides.Set(feature, true);
        const auto off_hash = HashFeatureSet(ResolveFeatureSet(off));
        const auto on_hash = HashFeatureSet(ResolveFeatureSet(on));
        REQUIRE(off_hash != on_hash);
        const std::array mapped_off{off_hash}, mapped_on{on_hash};
        REQUIRE(ComputeConfigHash(config, default_module, mapped_off) !=
                ComputeConfigHash(config, default_module, mapped_on));
        const std::array both{off_hash, on_hash}, reversed{on_hash, off_hash};
        REQUIRE(ComputeConfigHash(config, default_module, both) ==
                ComputeConfigHash(config, default_module, reversed));

        SerialUnit input{};
        input.guest_start = 0x2000;
        input.feature_hash = on_hash;
        input.is_function = 1;
        input.code = {0xc3, 0, 0, 0};
        BlobWriter writer;
        WriteUnit(writer, input);
        BlobReader reader{writer.Data().data(), writer.Size()};
        SerialUnit output{};
        REQUIRE(ReadUnit(reader, output));
        REQUIRE(reader.Empty());
        REQUIRE(output.guest_start == input.guest_start);
        REQUIRE(output.code == input.code);
        REQUIRE(output.feature_hash == HashFeatureSet(ResolveFeatureSet(on)));
        REQUIRE(output.feature_hash != HashFeatureSet(ResolveFeatureSet(off)));
        // This checks serialization and policy binding, without claiming a
        // disk-cache revive or executing the serialized host code.
    }
}
